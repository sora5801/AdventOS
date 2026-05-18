/*
 * virtio-blk driver.
 *
 * Wire protocol (virtio 1.0 §5.2):
 *
 *   struct virtio_blk_req {
 *       u32 type;          // 0 = IN (read), 1 = OUT (write), 4 = FLUSH
 *       u32 reserved;
 *       u64 sector;        // 512-byte units, regardless of device block size
 *       u8  data[len];     // payload (read fills, write provides)
 *       u8  status;        // device writes: 0=OK, 1=IOERR, 2=UNSUPP
 *   };
 *
 * A request is submitted as a 3-descriptor chain on virtqueue 0:
 *
 *   desc[0] -> header (device READS) — 16 bytes
 *   desc[1] -> data   (device READS for OUT, WRITES for IN)
 *   desc[2] -> status (device WRITES) — 1 byte
 *
 * Why three descriptors instead of one big buffer: the device needs
 * separate read-vs-write directions on the data buffer (OUT vs IN)
 * and a 1-byte status the device fills in last. Each direction =
 * one descriptor; chaining via VIRTQ_DESC_F_NEXT joins them.
 */
#include "virtio_blk.h"
#include "virtio.h"
#include "pci.h"
#include "blkdev.h"
#include "kmalloc.h"
#include "pmm.h"
#include "kprintf.h"
#include "string.h"
#include "spinlock.h"
#include "../include/io.h"

/* Request type. */
#define VBLK_T_IN     0
#define VBLK_T_OUT    1
#define VBLK_T_FLUSH  4

/* Status codes the device writes. */
#define VBLK_S_OK     0
#define VBLK_S_IOERR  1
#define VBLK_S_UNSUPP 2

/* Feature bits we care about. We don't ask for any — the legacy
 * defaults are fine. The host will advertise BLOCK_SIZE / GEOMETRY
 * etc.; we only consume CAPACITY (always present). */
#define VBLK_F_NONE   0

/* On-the-wire request header. 16 bytes packed. */
struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint32_t sector_lo;
    uint32_t sector_hi;
} __attribute__((packed));

#define VBLK_SECTOR_SIZE  512

/* Per-device state. */
struct vblk {
    int               in_use;
    struct pci_device pci;
    uint16_t          io;
    struct virtqueue  vq;
    uint64_t          capacity;     /* in 512-byte sectors */
    spinlock_t        lock;
    struct blkdev     bdev;
};

static struct vblk g_vblk;

/* ---- blkdev adapter -------------------------------------------- */

static int vblk_do_request(struct vblk *v, uint32_t lba, uint32_t n,
                           void *buf, int write)
{
    if (n == 0) return 0;
    /* Build the header. Static (single-LBA) buffer for the header
     * and status — we hold the device lock around each I/O, so
     * reuse is safe. */
    static struct virtio_blk_req hdr;
    static volatile uint8_t status;

    hdr.type      = write ? VBLK_T_OUT : VBLK_T_IN;
    hdr.reserved  = 0;
    hdr.sector_lo = lba;
    hdr.sector_hi = 0;
    status        = 0xFFu;       /* sentinel — device clears to 0/1/2 */

    spin_lock(&v->lock);

    /* Need three descriptors. */
    if (v->vq.n_free < 3) {
        spin_unlock(&v->lock);
        return -1;
    }
    int d0 = virtio_alloc_desc(&v->vq);
    int d1 = virtio_alloc_desc(&v->vq);
    int d2 = virtio_alloc_desc(&v->vq);
    if (d0 < 0 || d1 < 0 || d2 < 0) {
        if (d0 >= 0) virtio_free_desc_chain(&v->vq, (uint16_t)d0);
        if (d1 >= 0) virtio_free_desc_chain(&v->vq, (uint16_t)d1);
        if (d2 >= 0) virtio_free_desc_chain(&v->vq, (uint16_t)d2);
        spin_unlock(&v->lock);
        return -1;
    }

    /* desc[0] — header, device READS, chained to desc[1]. */
    v->vq.desc[d0].addr_lo = (uint32_t)(uintptr_t)&hdr;
    v->vq.desc[d0].addr_hi = 0;
    v->vq.desc[d0].len     = sizeof(hdr);
    v->vq.desc[d0].flags   = VIRTQ_DESC_F_NEXT;
    v->vq.desc[d0].next    = (uint16_t)d1;

    /* desc[1] — data, device READS for OUT, WRITES for IN. */
    v->vq.desc[d1].addr_lo = (uint32_t)(uintptr_t)buf;
    v->vq.desc[d1].addr_hi = 0;
    v->vq.desc[d1].len     = n * VBLK_SECTOR_SIZE;
    v->vq.desc[d1].flags   = (uint16_t)(VIRTQ_DESC_F_NEXT
                            | (write ? 0 : VIRTQ_DESC_F_WRITE));
    v->vq.desc[d1].next    = (uint16_t)d2;

    /* desc[2] — status, device WRITES. End of chain. */
    v->vq.desc[d2].addr_lo = (uint32_t)(uintptr_t)&status;
    v->vq.desc[d2].addr_hi = 0;
    v->vq.desc[d2].len     = 1;
    v->vq.desc[d2].flags   = VIRTQ_DESC_F_WRITE;
    v->vq.desc[d2].next    = 0;

    virtio_submit(v->io, &v->vq, (uint16_t)d0);

    /* Wait for completion (5 s deadline — generous for QEMU). */
    int rc = virtio_wait_used(v->io, &v->vq, 5000);
    spin_unlock(&v->lock);
    if (rc != 0) {
        kprintf("vblk: timeout on lba %u (n=%u, %s)\n",
                lba, n, write ? "write" : "read");
        return -1;
    }
    if (status != VBLK_S_OK) {
        kprintf("vblk: device error status=%u on lba %u\n",
                (unsigned)status, lba);
        return -1;
    }
    return 0;
}

static int vblk_blkdev_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf) {
    return vblk_do_request((struct vblk *)d->driver_data, lba, n, buf, 0);
}
static int vblk_blkdev_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf) {
    /* Cast away const — the data descriptor is OUT (device-read), so
     * the device never modifies the buffer. */
    return vblk_do_request((struct vblk *)d->driver_data, lba, n, (void *)buf, 1);
}

/* ---- init ------------------------------------------------------ */

void virtio_blk_init(void) {
    struct vblk *v = &g_vblk;
    if (v->in_use) return;

    if (pci_find(VIRTIO_VENDOR_ID, VIRTIO_LEGACY_BLK, &v->pci) != 0) {
        /* No virtio-blk present — silent (this is opt-in QEMU CLI). */
        return;
    }
    v->io = v->pci.io_base;
    spin_lock_init(&v->lock);

    kprintf("virtio-blk: PCI %u:%u.%u  io=0x%x  irq=%u  subsys=0x%x\n",
            v->pci.bus, v->pci.device, v->pci.func,
            (unsigned)v->io, v->pci.irq_line, v->pci.subsystem_id);

    /* Driver bring-up sequence per spec §3.1.1 / §4.1.5. */
    virtio_status_reset(v->io);
    virtio_status_ack(v->io);
    virtio_status_driver(v->io);

    uint32_t feats = virtio_negotiate(v->io, VBLK_F_NONE);
    (void)feats;     /* we don't react to any feature bits */

    /* One queue: virtqueue 0 is the request queue. */
    if (virtio_queue_init(v->io, 0, &v->vq) != 0) {
        kprintf("virtio-blk: queue 0 setup failed\n");
        virtio_status_failed(v->io);
        return;
    }

    virtio_status_driver_ok(v->io);

    /* Read capacity from device-specific config space. Legacy layout
     * starts at io+0x14 with no MSI; bytes 0..7 are the u64 capacity
     * in 512-byte sectors. */
    uint32_t cap_lo = inl(v->io + VIRTIO_PCI_CONFIG + 0);
    uint32_t cap_hi = inl(v->io + VIRTIO_PCI_CONFIG + 4);
    v->capacity = ((uint64_t)cap_hi << 32) | (uint64_t)cap_lo;
    kprintf("virtio-blk: capacity = %u sectors (%u KiB)\n",
            (unsigned)v->capacity, (unsigned)(v->capacity / 2));

    /* Register the blkdev. Use a 32-bit truncation for n_blocks — fine
     * for any sane test disk (this would only matter past 2 TiB). */
    v->bdev.block_size  = VBLK_SECTOR_SIZE;
    v->bdev.n_blocks    = (uint32_t)v->capacity;
    v->bdev.read        = vblk_blkdev_read;
    v->bdev.write       = vblk_blkdev_write;
    v->bdev.driver_data = v;
    /* Name. */
    const char *nm = "vblk0";
    int i;
    for (i = 0; nm[i] && i < BLKDEV_NAME_LEN - 1; i++) v->bdev.name[i] = nm[i];
    v->bdev.name[i] = 0;

    int slot = blkdev_register(&v->bdev);
    if (slot < 0) {
        kprintf("virtio-blk: blkdev table full — not registered\n");
        return;
    }
    kprintf("virtio-blk: registered as blkdev slot %d (%s)\n",
            slot, v->bdev.name);
    v->in_use = 1;

    /* Sanity-probe sector 0: confirms the request path works end-to-end
     * before any later bcache read trips on a half-broken driver. */
    static uint8_t probe[512];
    int rc = vblk_blkdev_read(&v->bdev, 0, 1, probe);
    if (rc == 0) {
        kprintf("virtio-blk: probe sector 0 ok (%02x %02x %02x %02x ...)\n",
                probe[0], probe[1], probe[2], probe[3]);
    } else {
        kprintf("virtio-blk: probe sector 0 FAILED — driver disabled\n");
        /* Mark the device unusable so bcache doesn't try later. */
        v->bdev.read  = 0;
        v->bdev.write = 0;
    }
}
