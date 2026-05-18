/*
 * virtio-balloon driver. See virtio_balloon.h for the QEMU CLI.
 *
 * Protocol (virtio 1.0 §5.5):
 *
 *   Config (device-specific, at offset 0x14 from BAR0):
 *     u32 num_pages  — host's request (target balloon size in 4K pages)
 *     u32 actual     — guest's report (current balloon size)
 *
 *   Queues:
 *     vq 0 (inflate) — guest gives pages to host
 *     vq 1 (deflate) — host gives pages back to guest
 *
 *   Each inflate/deflate request is a single descriptor pointing at
 *   a buffer of u32 PFNs. The host reads (inflate) or reads-and-
 *   uses (deflate) the PFNs and acknowledges by advancing used.idx.
 *
 *   The guest "actually" surrenders the pages by zeroing them and
 *   NOT touching them again — the host is then free to madvise(MADV_DONTNEED)
 *   or otherwise reclaim. Deflate is the inverse: the host won't
 *   touch the pages anymore, so the guest can use them.
 *
 *   The "actual" config field is the guest's own honesty signal —
 *   we update it after each successful inflate/deflate cycle.
 */
#include "virtio_balloon.h"
#include "virtio.h"
#include "pci.h"
#include "pmm.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"
#include "spinlock.h"
#include "pit.h"
#include "task.h"
#include "../include/io.h"

#define BALLOON_MAX_PAGES   512      /* 2 MiB cap */
#define BATCH_MAX           64       /* PFNs per virtio request */

struct vballoon {
    int               in_use;
    struct pci_device pci;
    uint16_t          io;
    struct virtqueue  inflate_vq;     /* qidx 0 */
    struct virtqueue  deflate_vq;     /* qidx 1 */
    spinlock_t        lock;
    /* Ballooned page numbers, kmalloc'd at init to keep .bss small.
     * Ordered by allocation order — deflate frees LIFO. */
    uint32_t         *pfns;
    uint32_t          actual_pages;
    uint32_t          target_pages;
    uint32_t          total_inflated;
    uint32_t          total_deflated;
    /* PFN buffer for one virtio request — kernel-side scratch. */
    uint32_t         *pfn_buf;
};

static struct vballoon g_vb;

/* Read num_pages from config space at io+0x14. Little-endian u32. */
static uint32_t read_target(struct vballoon *v) {
    return inl(v->io + VIRTIO_PCI_CONFIG + 0);
}

/* Write our `actual` count back to config space at io+0x14+4. */
static void write_actual(struct vballoon *v, uint32_t n) {
    outl(v->io + VIRTIO_PCI_CONFIG + 4, n);
}

/* Submit one batch of PFNs via the given queue (inflate or deflate). */
static int send_batch(struct vballoon *v, struct virtqueue *vq,
                      const uint32_t *pfns, int n)
{
    /* Copy into the kernel scratch (descriptor must point at a
     * stable, physically-contiguous buffer). */
    for (int i = 0; i < n; i++) v->pfn_buf[i] = pfns[i];

    int d0 = virtio_alloc_desc(vq);
    if (d0 < 0) return -1;

    /* Single descriptor: device READS the PFN array. */
    vq->desc[d0].addr_lo = (uint32_t)(uintptr_t)v->pfn_buf;
    vq->desc[d0].addr_hi = 0;
    vq->desc[d0].len     = (uint32_t)(n * 4);
    vq->desc[d0].flags   = 0;
    vq->desc[d0].next    = 0;

    virtio_submit(v->io, vq, (uint16_t)d0);
    return virtio_wait_used(v->io, vq, 2000);
}

/* Inflate by `n` pages: allocate from PMM, append to v->pfns, send. */
static int do_inflate(struct vballoon *v, int n) {
    if (v->actual_pages + (uint32_t)n > BALLOON_MAX_PAGES) {
        n = (int)(BALLOON_MAX_PAGES - v->actual_pages);
    }
    if (n <= 0) return 0;
    if (n > BATCH_MAX) n = BATCH_MAX;

    uint32_t batch[BATCH_MAX];
    int got = 0;
    for (int i = 0; i < n; i++) {
        void *p = pmm_alloc_page();
        if (!p) break;
        batch[got] = (uint32_t)(uintptr_t)p >> 12;
        v->pfns[v->actual_pages + got] = batch[got];
        got++;
    }
    if (got == 0) return 0;

    if (send_batch(v, &v->inflate_vq, batch, got) != 0) {
        /* Send failed — give the pages back to PMM to avoid leaking. */
        for (int i = 0; i < got; i++) {
            pmm_free_page((void *)(uintptr_t)(batch[i] << 12));
        }
        return -1;
    }

    v->actual_pages    += (uint32_t)got;
    v->total_inflated  += (uint32_t)got;
    write_actual(v, v->actual_pages);
    return got;
}

/* Deflate by `n` pages: pop from the tail of v->pfns, send, free. */
static int do_deflate(struct vballoon *v, int n) {
    if ((uint32_t)n > v->actual_pages) n = (int)v->actual_pages;
    if (n <= 0) return 0;
    if (n > BATCH_MAX) n = BATCH_MAX;

    uint32_t batch[BATCH_MAX];
    for (int i = 0; i < n; i++) {
        batch[i] = v->pfns[v->actual_pages - 1 - i];
    }

    if (send_batch(v, &v->deflate_vq, batch, n) != 0) return -1;

    /* Host has acknowledged; the pages are ours again. Return them to
     * PMM and shrink our tracking array. */
    for (int i = 0; i < n; i++) {
        pmm_free_page((void *)(uintptr_t)(batch[i] << 12));
    }
    v->actual_pages    -= (uint32_t)n;
    v->total_deflated  += (uint32_t)n;
    write_actual(v, v->actual_pages);
    return n;
}

/* Periodic check: compare config.num_pages to our actual; adjust. */
static void virtio_balloon_task(void) {
    struct vballoon *v = &g_vb;
    for (;;) {
        spin_lock(&v->lock);
        v->target_pages = read_target(v);
        uint32_t target = v->target_pages;
        uint32_t actual = v->actual_pages;
        spin_unlock(&v->lock);

        if (target > BALLOON_MAX_PAGES) target = BALLOON_MAX_PAGES;

        if (target > actual) {
            int want = (int)(target - actual);
            spin_lock(&v->lock);
            int did = do_inflate(v, want);
            spin_unlock(&v->lock);
            if (did > 0) {
                kprintf("virtio-balloon: inflated %d pages "
                        "(actual=%u target=%u)\n",
                        did, (unsigned)v->actual_pages,
                        (unsigned)v->target_pages);
            }
        } else if (target < actual) {
            int want = (int)(actual - target);
            spin_lock(&v->lock);
            int did = do_deflate(v, want);
            spin_unlock(&v->lock);
            if (did > 0) {
                kprintf("virtio-balloon: deflated %d pages "
                        "(actual=%u target=%u)\n",
                        did, (unsigned)v->actual_pages,
                        (unsigned)v->target_pages);
            }
        }
        /* 1 Hz is plenty — balloon target rarely changes; the QEMU
         * `balloon` monitor command updates it on demand. */
        pit_sleep(1000);
    }
}

void virtio_balloon_start_task(void) {
    if (!g_vb.in_use) return;
    task_make_runnable(task_create(virtio_balloon_task, "virtio-balloon"));
    kprintf("virtio-balloon: cooperation task started\n");
}

int virtio_balloon_get_stats(uint32_t out[4]) {
    if (!g_vb.in_use) return -1;
    out[0] = g_vb.actual_pages;
    out[1] = g_vb.target_pages;
    out[2] = g_vb.total_inflated;
    out[3] = g_vb.total_deflated;
    return 0;
}

void virtio_balloon_init(void) {
    struct vballoon *v = &g_vb;
    if (v->in_use) return;

    if (pci_find(VIRTIO_VENDOR_ID, VIRTIO_LEGACY_BALLOON, &v->pci) != 0) {
        return;
    }
    v->io = v->pci.io_base;
    spin_lock_init(&v->lock);

    kprintf("virtio-balloon: PCI %u:%u.%u  io=0x%x  irq=%u\n",
            v->pci.bus, v->pci.device, v->pci.func,
            (unsigned)v->io, v->pci.irq_line);

    virtio_status_reset(v->io);
    virtio_status_ack(v->io);
    virtio_status_driver(v->io);
    virtio_negotiate(v->io, 0);

    if (virtio_queue_init(v->io, 0, &v->inflate_vq) != 0 ||
        virtio_queue_init(v->io, 1, &v->deflate_vq) != 0)
    {
        kprintf("virtio-balloon: queue setup failed\n");
        virtio_status_failed(v->io);
        return;
    }
    virtio_install_irq(v->io, v->pci.irq_line, 0, v);
    virtio_status_driver_ok(v->io);

    /* Heap allocations: pfn tracking + per-request scratch. */
    v->pfns = kmalloc(BALLOON_MAX_PAGES * sizeof(uint32_t));
    v->pfn_buf = kmalloc(BATCH_MAX * sizeof(uint32_t));
    if (!v->pfns || !v->pfn_buf) {
        kprintf("virtio-balloon: scratch alloc failed\n");
        return;
    }

    v->actual_pages    = 0;
    v->target_pages    = read_target(v);
    v->total_inflated  = 0;
    v->total_deflated  = 0;
    write_actual(v, 0);

    v->in_use = 1;
    kprintf("virtio-balloon: ready (target=%u pages)\n",
            (unsigned)v->target_pages);
}
