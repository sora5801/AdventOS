/*
 * virtio common — see virtio.h.
 *
 * The legacy PCI transport requires:
 *   - queue addresses are physical page numbers (phys >> 12)
 *   - the desc table + avail ring + used ring live in one contiguous
 *     physical region; the host reads the PFN of the descriptor table
 *     and computes the avail/used offsets per spec
 *   - avail ring follows immediately after the desc table
 *   - used ring starts on the next page boundary after the avail ring
 *
 * For a queue of size N the descriptor table is 16*N bytes; the avail
 * ring is 6 + 2*N bytes; the used ring is 6 + 8*N bytes. With N = 64
 * the desc+avail fits in 1158 bytes (under 4 KiB) and the used ring
 * fits in 522 bytes — total 2 pages worth (8 KiB) of contiguous PMM.
 *
 * Each driver picks its qsize (we read whatever QEMU advertises and
 * fall back to 64 if the advertised number is bigger than our cap).
 */
#include "virtio.h"
#include "pci.h"
#include "pmm.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"
#include "../include/io.h"

/* In legacy virtio, queue size is set by the device (QUEUE_NUM is
 * read-only). The driver MUST use exactly that size — silently
 * truncating would leave QEMU computing avail/used base offsets
 * from the device's qsize and reading garbage. Modern QEMU defaults:
 *   virtio-blk = 128
 *   virtio-net = 256
 * Our cap protects against absurdly large queues but is large enough
 * to accept those defaults verbatim. */
#define VIRTIO_QSIZE_MAX  256

/* ---- status register ------------------------------------------- */

void virtio_status_reset(uint16_t io_base) {
    /* Writing 0 resets the device per spec §3.1.1. */
    outb(io_base + VIRTIO_PCI_STATUS, 0);
}

static void status_set(uint16_t io_base, uint8_t add_bits) {
    uint8_t s = inb(io_base + VIRTIO_PCI_STATUS);
    outb(io_base + VIRTIO_PCI_STATUS, (uint8_t)(s | add_bits));
}

void virtio_status_ack      (uint16_t io_base) { status_set(io_base, VIRTIO_STATUS_ACKNOWLEDGE); }
void virtio_status_driver   (uint16_t io_base) { status_set(io_base, VIRTIO_STATUS_DRIVER); }
void virtio_status_driver_ok(uint16_t io_base) { status_set(io_base, VIRTIO_STATUS_DRIVER_OK); }
void virtio_status_failed   (uint16_t io_base) { status_set(io_base, VIRTIO_STATUS_FAILED); }

/* ---- feature negotiation --------------------------------------- */

uint32_t virtio_negotiate(uint16_t io_base, uint32_t wanted) {
    uint32_t host = inl(io_base + VIRTIO_PCI_HOST_FEATURES);
    uint32_t common = host & wanted;
    outl(io_base + VIRTIO_PCI_GUEST_FEATURES, common);
    return common;
}

/* ---- queue setup ----------------------------------------------- */

/* Compute the per-queue ring footprint. The caller allocates that
 * many bytes (rounded up to a page). Spec layout (legacy):
 *   desc table | avail ring | (pad to 4 KiB) | used ring */
static void compute_layout(uint16_t qsize,
                           uint32_t *desc_bytes,
                           uint32_t *avail_bytes,
                           uint32_t *avail_padded,
                           uint32_t *used_bytes,
                           uint32_t *total_bytes)
{
    uint32_t desc  = (uint32_t)qsize * 16u;
    uint32_t avail = 6u + 2u * (uint32_t)qsize;        /* + 2 if event_idx */
    uint32_t used  = 6u + 8u * (uint32_t)qsize;        /* + 2 if event_idx */
    uint32_t avail_end = desc + avail;
    uint32_t used_off  = (avail_end + 0xFFFu) & ~0xFFFu;
    uint32_t total     = used_off + used;
    *desc_bytes   = desc;
    *avail_bytes  = avail;
    *avail_padded = used_off;
    *used_bytes   = used;
    *total_bytes  = total;
}

int virtio_queue_init(uint16_t io_base, uint16_t qidx, struct virtqueue *vq) {
    /* Tell the device which queue we want to configure, then read its
     * advertised size. QEMU's defaults are 128 (blk) and 256 (net) —
     * we cap at VIRTIO_QSIZE_MAX to keep memory use sane. */
    outw(io_base + VIRTIO_PCI_QUEUE_SEL, qidx);
    uint16_t qsize_hw = inw(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (qsize_hw == 0) return -1;
    uint16_t qsize = qsize_hw;
    if (qsize > VIRTIO_QSIZE_MAX) qsize = VIRTIO_QSIZE_MAX;

    uint32_t desc_bytes, avail_bytes, avail_padded, used_bytes, total_bytes;
    compute_layout(qsize, &desc_bytes, &avail_bytes,
                   &avail_padded, &used_bytes, &total_bytes);

    /* Allocate ring storage from the kmalloc heap (identity-mapped,
     * always above 0x100000 and well clear of any low-memory pages
     * the firmware might still touch). Round up to whole pages plus
     * one extra for alignment slack. */
    uint32_t total_pages = (total_bytes + 0xFFFu) >> 12;
    if (total_pages < 1) total_pages = 1;
    uint32_t alloc_bytes = (total_pages + 1) * 4096u;
    uint8_t *raw = (uint8_t *)kmalloc(alloc_bytes);
    if (!raw) {
        kprintf("virtio: qidx %u kmalloc(%u) failed\n", qidx, alloc_bytes);
        return -1;
    }
    uintptr_t aligned = ((uintptr_t)raw + 0xFFFu) & ~0xFFFu;
    void *ring = (void *)aligned;
    memset(ring, 0, total_pages * 4096u);

    vq->qsize     = qsize;
    vq->qidx      = qidx;
    vq->desc      = (struct virtq_desc  *)ring;
    vq->avail     = (struct virtq_avail *)((uint8_t *)ring + desc_bytes);
    vq->used      = (struct virtq_used  *)((uint8_t *)ring + avail_padded);
    vq->ring_phys = (uint32_t)(uintptr_t)ring;
    vq->last_used = 0;

    /* Thread all descriptors into a free list via the `next` field. */
    for (uint16_t i = 0; i < qsize; i++) {
        vq->desc[i].next = (uint16_t)(i + 1);
    }
    vq->desc[qsize - 1].next = 0xFFFFu;
    vq->free_head = 0;
    vq->n_free    = qsize;

    /* Tell the host the queue base. Legacy: write PFN = phys >> 12. */
    outl(io_base + VIRTIO_PCI_QUEUE_PFN,
         (uint32_t)((uintptr_t)ring >> 12));

    return 0;
}

/* ---- descriptor pool ------------------------------------------- */

int virtio_alloc_desc(struct virtqueue *vq) {
    if (vq->n_free == 0) return -1;
    uint16_t head = vq->free_head;
    vq->free_head = vq->desc[head].next;
    vq->n_free--;
    /* Caller will overwrite next; clear it for safety. */
    vq->desc[head].next  = 0;
    vq->desc[head].flags = 0;
    return head;
}

void virtio_free_desc_chain(struct virtqueue *vq, uint16_t head) {
    /* Walk the chain by NEXT pointers, prepending each to the free
     * list. The descriptor's next field is reused as the free-list
     * link — the avail ring keeps its own copy of the head index. */
    uint16_t i = head;
    for (;;) {
        uint16_t nxt   = vq->desc[i].next;
        int has_next   = vq->desc[i].flags & VIRTQ_DESC_F_NEXT;
        vq->desc[i].next = vq->free_head;
        vq->free_head    = i;
        vq->n_free++;
        if (!has_next) break;
        i = nxt;
    }
}

/* ---- submit / wait --------------------------------------------- */

void virtio_submit(uint16_t io_base, struct virtqueue *vq, uint16_t head) {
    /* Spec §2.4.6: write desc head into avail.ring[avail.idx % qsize],
     * memory barrier, then bump avail.idx. We're on a single CPU and
     * the device sees physical memory directly, so a compiler barrier
     * (volatile reads/writes via the struct) is enough. */
    uint16_t slot = (uint16_t)(vq->avail->idx % vq->qsize);
    vq->avail->ring[slot] = head;
    /* Compiler barrier so the slot write isn't reordered past idx. */
    __asm__ volatile ("" ::: "memory");
    vq->avail->idx = (uint16_t)(vq->avail->idx + 1);
    /* Notify the host. Some devices set NO_NOTIFY in used.flags
     * once they're polling; for simplicity we always kick. */
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, vq->qidx);
}

int virtio_wait_used(uint16_t io_base, struct virtqueue *vq,
                     uint32_t timeout_ms)
{
    uint32_t start = pit_ticks();
    uint32_t deadline_ticks = (timeout_ms * 100u) / 1000u;
    if (deadline_ticks == 0) deadline_ticks = 1;

    /* used->idx is updated by the host out-of-band w.r.t. the guest
     * CPU pipeline, so dereference it as volatile every iteration —
     * otherwise gcc happily hoists the load out of the loop and we
     * spin forever on a stale snapshot. */
    volatile uint16_t *p_used_idx = &vq->used->idx;

    while ((uint16_t)(*p_used_idx - vq->last_used) == 0) {
        /* Yield to the host so its virtio-blk/net coroutines run. The
         * pushfl/popfl pair makes this transparent to the caller's
         * IF state (we may be invoked with IRQs off from init code
         * or on from a task). */
        __asm__ volatile (
            "pushfl\n\t"
            "sti\n\t"
            "hlt\n\t"
            "popfl\n\t"
            ::: "memory", "cc"
        );
        /* Drain ISR so the device IRQ latch clears. */
        (void)inb(io_base + VIRTIO_PCI_ISR);
        if (pit_ticks() - start > deadline_ticks) return -1;
    }

    uint16_t slot = (uint16_t)(vq->last_used % vq->qsize);
    uint16_t head = (uint16_t)vq->used->ring[slot].id;
    virtio_free_desc_chain(vq, head);
    vq->last_used = (uint16_t)(vq->last_used + 1);
    return 0;
}
