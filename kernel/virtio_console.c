/*
 * virtio-console driver. See virtio_console.h for the QEMU CLI.
 *
 * RX strategy: pre-arm N_RX_BUFS descriptors of RX_BUF_SIZE each.
 * A polling task wakes every 50 ms, drains the used ring, copies
 * incoming bytes into g_rx_ring, and re-arms each consumed
 * descriptor.
 *
 * TX strategy: per-call, allocate a descriptor, copy from caller's
 * buffer into a kernel-side scratch, submit, wait, free.
 */
#include "virtio_console.h"
#include "virtio.h"
#include "pci.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"
#include "spinlock.h"
#include "pit.h"
#include "task.h"
#include "../include/io.h"

#define N_RX_BUFS      4
#define RX_BUF_SIZE    512
#define RX_RING_SIZE   2048       /* in-kernel byte ring drained by SYS_*READ */

struct vcons {
    int               in_use;
    struct pci_device pci;
    uint16_t          io;
    struct virtqueue  rx_vq;       /* qidx 0 */
    struct virtqueue  tx_vq;       /* qidx 1 */
    uint8_t          *rx_buf[N_RX_BUFS];
    spinlock_t        tx_lock;
    uint8_t          *tx_buf;      /* RX_BUF_SIZE scratch */
    /* In-kernel byte ring drained by SYS_VIRTIO_CONSOLE_READ. Kept
     * in the kmalloc heap (not .bss) so this struct stays compact. */
    spinlock_t        ring_lock;
    uint8_t          *ring;        /* RX_RING_SIZE bytes from kmalloc */
    uint32_t          ring_head;   /* producer (where next byte goes) */
    uint32_t          ring_tail;   /* consumer (next byte to read out) */
};

static struct vcons g_vcons;

/* Push a freshly-armed empty buffer onto the RX avail ring. */
static void rx_arm(struct vcons *v, int idx) {
    v->rx_vq.desc[idx].addr_lo = (uint32_t)(uintptr_t)v->rx_buf[idx];
    v->rx_vq.desc[idx].addr_hi = 0;
    v->rx_vq.desc[idx].len     = RX_BUF_SIZE;
    v->rx_vq.desc[idx].flags   = VIRTQ_DESC_F_WRITE;
    v->rx_vq.desc[idx].next    = 0;

    uint16_t slot = (uint16_t)(v->rx_vq.avail->idx % v->rx_vq.qsize);
    v->rx_vq.avail->ring[slot] = (uint16_t)idx;
    v->rx_vq.avail->idx        = (uint16_t)(v->rx_vq.avail->idx + 1);
}

/* Copy `len` bytes from `src` into the in-kernel ring, dropping any
 * overflow rather than blocking (matches keyboard ring semantics). */
static void ring_push(struct vcons *v, const uint8_t *src, int len) {
    spin_lock(&v->ring_lock);
    for (int i = 0; i < len; i++) {
        uint32_t next = (v->ring_head + 1) % RX_RING_SIZE;
        if (next == v->ring_tail) break;     /* full; drop the rest */
        v->ring[v->ring_head] = src[i];
        v->ring_head = next;
    }
    spin_unlock(&v->ring_lock);
}

static int ring_pop(struct vcons *v, uint8_t *dst, int max) {
    int got = 0;
    spin_lock(&v->ring_lock);
    while (got < max && v->ring_tail != v->ring_head) {
        dst[got++] = v->ring[v->ring_tail];
        v->ring_tail = (v->ring_tail + 1) % RX_RING_SIZE;
    }
    spin_unlock(&v->ring_lock);
    return got;
}

static void rx_drain(struct vcons *v) {
    while ((uint16_t)(v->rx_vq.used->idx - v->rx_vq.last_used) != 0) {
        uint16_t slot = (uint16_t)(v->rx_vq.last_used % v->rx_vq.qsize);
        struct virtq_used_elem ue = v->rx_vq.used->ring[slot];
        uint16_t idx = (uint16_t)ue.id;
        uint32_t len = ue.len;
        if (idx < N_RX_BUFS && len > 0) {
            if (len > RX_BUF_SIZE) len = RX_BUF_SIZE;
            ring_push(v, v->rx_buf[idx], (int)len);
        }
        rx_arm(v, idx);
        v->rx_vq.last_used = (uint16_t)(v->rx_vq.last_used + 1);
    }
    /* One kick to notify the host we've re-armed descriptors. */
    outw(v->io + VIRTIO_PCI_QUEUE_NOTIFY, v->rx_vq.qidx);
}

static void virtio_console_rx_task(void) {
    struct vcons *v = &g_vcons;
    for (;;) {
        rx_drain(v);
        pit_sleep(50);
    }
}

void virtio_console_start_polling(void) {
    if (!g_vcons.in_use) return;
    task_make_runnable(task_create(virtio_console_rx_task, "vcons-rx"));
    kprintf("virtio-console: RX polling task started\n");
}

int virtio_console_write(const void *buf, int n) {
    struct vcons *v = &g_vcons;
    if (!v->in_use) return -1;
    if (n <= 0 || n > RX_BUF_SIZE) return -1;

    spin_lock(&v->tx_lock);
    memcpy(v->tx_buf, buf, n);

    /* If the TX queue has stale completions, drain them so we don't
     * exhaust descriptors. */
    while ((uint16_t)(v->tx_vq.used->idx - v->tx_vq.last_used) != 0) {
        uint16_t slot = (uint16_t)(v->tx_vq.last_used % v->tx_vq.qsize);
        uint16_t head = (uint16_t)v->tx_vq.used->ring[slot].id;
        virtio_free_desc_chain(&v->tx_vq, head);
        v->tx_vq.last_used = (uint16_t)(v->tx_vq.last_used + 1);
    }

    int d0 = virtio_alloc_desc(&v->tx_vq);
    if (d0 < 0) { spin_unlock(&v->tx_lock); return -1; }

    v->tx_vq.desc[d0].addr_lo = (uint32_t)(uintptr_t)v->tx_buf;
    v->tx_vq.desc[d0].addr_hi = 0;
    v->tx_vq.desc[d0].len     = (uint32_t)n;
    v->tx_vq.desc[d0].flags   = 0;     /* device READS */
    v->tx_vq.desc[d0].next    = 0;

    virtio_submit(v->io, &v->tx_vq, (uint16_t)d0);
    int rc = virtio_wait_used(v->io, &v->tx_vq, 1000);
    spin_unlock(&v->tx_lock);
    if (rc != 0) {
        kprintf("virtio-console: TX timeout, n=%d\n", n);
        return -1;
    }
    return n;
}

int virtio_console_read(void *buf, int n) {
    struct vcons *v = &g_vcons;
    if (!v->in_use) return -1;
    return ring_pop(v, (uint8_t *)buf, n);
}

void virtio_console_init(void) {
    struct vcons *v = &g_vcons;
    if (v->in_use) return;

    if (pci_find(VIRTIO_VENDOR_ID, VIRTIO_LEGACY_CONSOLE, &v->pci) != 0) {
        return;
    }
    v->io = v->pci.io_base;
    spin_lock_init(&v->tx_lock);
    spin_lock_init(&v->ring_lock);

    kprintf("virtio-console: PCI %u:%u.%u  io=0x%x  irq=%u\n",
            v->pci.bus, v->pci.device, v->pci.func,
            (unsigned)v->io, v->pci.irq_line);

    virtio_status_reset(v->io);
    virtio_status_ack(v->io);
    virtio_status_driver(v->io);
    /* Ack no features — port 0 default works without MULTIPORT or
     * SIZE or EMERG_WRITE. */
    virtio_negotiate(v->io, 0);

    if (virtio_queue_init(v->io, 0, &v->rx_vq) != 0 ||
        virtio_queue_init(v->io, 1, &v->tx_vq) != 0)
    {
        kprintf("virtio-console: queue setup failed\n");
        virtio_status_failed(v->io);
        return;
    }
    virtio_status_driver_ok(v->io);

    /* RX buffers. */
    for (int i = 0; i < N_RX_BUFS; i++) {
        v->rx_buf[i] = kmalloc(RX_BUF_SIZE);
        if (!v->rx_buf[i]) {
            kprintf("virtio-console: RX alloc failed\n");
            return;
        }
    }
    /* RX descriptors are pre-armed by us; the virtio_queue_init
     * free list is irrelevant for the RX queue. */
    v->rx_vq.free_head = 0xFFFFu;
    v->rx_vq.n_free    = 0;
    for (int i = 0; i < N_RX_BUFS; i++) rx_arm(v, i);
    outw(v->io + VIRTIO_PCI_QUEUE_NOTIFY, v->rx_vq.qidx);

    /* TX scratch buffer. */
    v->tx_buf = kmalloc(RX_BUF_SIZE);
    if (!v->tx_buf) {
        kprintf("virtio-console: TX alloc failed\n");
        return;
    }
    v->ring = kmalloc(RX_RING_SIZE);
    if (!v->ring) {
        kprintf("virtio-console: RX ring alloc failed\n");
        return;
    }
    v->ring_head = v->ring_tail = 0;
    v->in_use = 1;

    /* Sanity probe: TX a hello banner. If the chardev backing the
     * console is a socket/file/null sink, this verifies the TX path
     * end-to-end. */
    const char hello[] = "virtio-console: AdventOS hvc0 online\r\n";
    int sent = virtio_console_write(hello, (int)(sizeof(hello) - 1));
    if (sent > 0) {
        kprintf("virtio-console: probe ok (TX %d bytes)\n", sent);
    } else {
        kprintf("virtio-console: probe FAILED (TX rc=%d)\n", sent);
        v->in_use = 0;
    }
}
