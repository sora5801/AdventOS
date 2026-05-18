/*
 * virtio-rng driver. The simplest virtio device in existence — one
 * queue, no feature bits, no device-specific config. Each request
 * is a single writable descriptor: the device fills it with bytes
 * and bumps used.idx.
 *
 * QEMU caps each request at 32 bytes by default (the rate-limiting
 * bytes-per-period config), but multiple requests can be queued back
 * to back to amortize the round trip. We do them one at a time for
 * simplicity — entropy is rarely needed in tight loops.
 */
#include "virtio_rng.h"
#include "virtio.h"
#include "pci.h"
#include "kprintf.h"
#include "string.h"
#include "spinlock.h"
#include "../include/io.h"

struct vrng {
    int               in_use;
    struct pci_device pci;
    uint16_t          io;
    struct virtqueue  vq;
    spinlock_t        lock;
};

static struct vrng g_vrng;

void virtio_rng_init(void) {
    struct vrng *v = &g_vrng;
    if (v->in_use) return;

    if (pci_find(VIRTIO_VENDOR_ID, VIRTIO_LEGACY_RNG, &v->pci) != 0) {
        /* Not present — silent (opt-in QEMU CLI). */
        return;
    }
    v->io = v->pci.io_base;
    spin_lock_init(&v->lock);

    kprintf("virtio-rng: PCI %u:%u.%u  io=0x%x  irq=%u\n",
            v->pci.bus, v->pci.device, v->pci.func,
            (unsigned)v->io, v->pci.irq_line);

    virtio_status_reset(v->io);
    virtio_status_ack(v->io);
    virtio_status_driver(v->io);
    virtio_negotiate(v->io, 0);

    if (virtio_queue_init(v->io, 0, &v->vq) != 0) {
        kprintf("virtio-rng: queue 0 setup failed\n");
        virtio_status_failed(v->io);
        return;
    }
    virtio_status_driver_ok(v->io);

    v->in_use = 1;

    /* Sanity probe: pull 8 bytes and dump them. Confirms the request
     * round-trip works before any later caller depends on it. */
    uint8_t probe[8];
    int got = virtio_rng_get(probe, sizeof(probe));
    if (got == (int)sizeof(probe)) {
        kprintf("virtio-rng: probe ok (%02x %02x %02x %02x %02x %02x %02x %02x)\n",
                probe[0], probe[1], probe[2], probe[3],
                probe[4], probe[5], probe[6], probe[7]);
    } else {
        kprintf("virtio-rng: probe FAILED (got %d)\n", got);
        v->in_use = 0;
    }
}

int virtio_rng_available(void) {
    return g_vrng.in_use;
}

int virtio_rng_get(void *buf, int len) {
    struct vrng *v = &g_vrng;
    if (!v->in_use) return -1;
    if (len <= 0) return 0;

    spin_lock(&v->lock);

    int d0 = virtio_alloc_desc(&v->vq);
    if (d0 < 0) { spin_unlock(&v->lock); return -1; }

    /* Single descriptor, writable by the host: virtio-rng has no
     * header, the entire buffer is just "give me this many random
     * bytes". */
    v->vq.desc[d0].addr_lo = (uint32_t)(uintptr_t)buf;
    v->vq.desc[d0].addr_hi = 0;
    v->vq.desc[d0].len     = (uint32_t)len;
    v->vq.desc[d0].flags   = VIRTQ_DESC_F_WRITE;
    v->vq.desc[d0].next    = 0;

    /* Remember the used.idx slot we'll consume so we can read the
     * actual count out of it before virtio_wait_used frees the
     * descriptor — the host may have written fewer than `len` bytes
     * if its rate-limiter kicked in. */
    uint16_t slot = (uint16_t)(v->vq.last_used % v->vq.qsize);
    virtio_submit(v->io, &v->vq, (uint16_t)d0);

    int rc = virtio_wait_used(v->io, &v->vq, 2000);
    if (rc != 0) {
        spin_unlock(&v->lock);
        kprintf("virtio-rng: get timed out\n");
        return -1;
    }
    int got = (int)v->vq.used->ring[slot].len;
    if (got > len) got = len;     /* defensive */
    spin_unlock(&v->lock);
    return got;
}
