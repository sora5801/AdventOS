/*
 * virtio-scsi driver. See virtio_scsi.h.
 *
 * Wire format (virtio 1.0 §5.6):
 *
 *   struct virtio_scsi_req_cmd {
 *     // -- device READS:
 *     u8  lun[8];           // SAM LUN, with lun[1] = target id
 *     u64 tag;              // request id; device echoes in response
 *     u8  task_attr;        // 0 = SIMPLE
 *     u8  prio;             // 0
 *     u8  crn;              // 0
 *     u8  cdb[32];          // SCSI Command Block (we use 10-byte CDBs)
 *     ...                    // followed by data-out for WRITE
 *
 *     // -- device WRITES:
 *     u32 sense_len;
 *     u32 residual;
 *     u16 status_qualifier;
 *     u8  status;            // 0 = GOOD
 *     u8  response;          // 0 = COMPLETED
 *     u8  sense[96];
 *     ...                    // followed by data-in for READ
 *   };
 *
 * Three virtqueues: control (vq 0), event (vq 1), request (vq 2).
 * We only ever use the request queue; we never enable events.
 *
 * Per request the driver builds a chain:
 *
 *   For READ(10):
 *     desc[0] -> req cmd header           (device READS, 51 bytes
 *                                          before the CDB tail)
 *     desc[1] -> req cmd response header  (device WRITES, 96+10 bytes)
 *     desc[2] -> data-in buffer           (device WRITES, n*512)
 *
 *   For WRITE(10):
 *     desc[0] -> req cmd header           (device READS, with CDB)
 *     desc[1] -> data-out buffer          (device READS, n*512)
 *     desc[2] -> req cmd response header  (device WRITES)
 *
 * We use READ(10) / WRITE(10) for 32-bit LBA — caps at 2 TiB which is
 * plenty for everything AdventOS will ever see.
 */
#include "virtio_scsi.h"
#include "virtio.h"
#include "pci.h"
#include "blkdev.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"
#include "spinlock.h"
#include "../include/io.h"

/* On-the-wire request header (cmd half — what we send). */
struct vscsi_req_cmd_hdr {
    uint8_t  lun[8];
    uint64_t tag;
    uint8_t  task_attr;
    uint8_t  prio;
    uint8_t  crn;
    uint8_t  cdb[32];
} __attribute__((packed));

/* On-the-wire response header (resp half — what the device writes). */
struct vscsi_req_resp_hdr {
    uint32_t sense_len;
    uint32_t residual;
    uint16_t status_qualifier;
    uint8_t  status;
    uint8_t  response;
    uint8_t  sense[96];
} __attribute__((packed));

#define VSCSI_STATUS_GOOD       0
#define VSCSI_RESPONSE_OK       0

/* Per-device state. */
struct vscsi {
    int               in_use;
    struct pci_device pci;
    uint16_t          io;
    struct virtqueue  ctrl_vq;       /* qidx 0 (unused) */
    struct virtqueue  event_vq;      /* qidx 1 (unused) */
    struct virtqueue  req_vq;        /* qidx 2 */
    spinlock_t        lock;
    /* Per-request scratch (one in-flight). */
    struct vscsi_req_cmd_hdr  cmd;
    struct vscsi_req_resp_hdr resp;
    /* Target/LUN of the disk we discovered. SAM 8-byte LUN. */
    uint8_t           lun[8];
    uint32_t          n_blocks;
    uint32_t          block_size;
    struct blkdev     bdev;
};

static struct vscsi g_vscsi;

/* ---- SCSI command helpers --------------------------------------- */

/* Send one SCSI command through the request virtqueue. cdb is the
 * 10-byte command. data_buf is the data buffer; data_in selects
 * direction. Returns 0 on GOOD status, -1 otherwise. */
static int vscsi_command(struct vscsi *v,
                         const uint8_t *cdb, int cdb_len,
                         void *data_buf, uint32_t data_len,
                         int data_in)
{
    spin_lock(&v->lock);

    /* Fill the request header in our per-device scratch. */
    memset(&v->cmd, 0, sizeof(v->cmd));
    for (int i = 0; i < 8; i++) v->cmd.lun[i] = v->lun[i];
    v->cmd.tag = 0;                 /* single in-flight; tag unused */
    v->cmd.task_attr = 0;
    for (int i = 0; i < cdb_len && i < 32; i++) v->cmd.cdb[i] = cdb[i];

    /* Clear the response slot — the device writes here. */
    memset(&v->resp, 0xFF, sizeof(v->resp));

    /* Build a 3-descriptor chain. The READ vs WRITE orderings are:
     *   READ : [cmd READ] [resp WRITE] [data WRITE]
     *   WRITE: [cmd READ] [data READ ] [resp WRITE]
     * In both cases the device-readable descriptors come first and
     * the device-writable descriptors come second, in chain order. */
    int d0 = virtio_alloc_desc(&v->req_vq);
    int d1 = virtio_alloc_desc(&v->req_vq);
    int d2 = (data_len > 0) ? virtio_alloc_desc(&v->req_vq) : -1;
    if (d0 < 0 || d1 < 0 || (data_len > 0 && d2 < 0)) {
        if (d0 >= 0) virtio_free_desc_chain(&v->req_vq, (uint16_t)d0);
        if (d1 >= 0) virtio_free_desc_chain(&v->req_vq, (uint16_t)d1);
        if (d2 >= 0) virtio_free_desc_chain(&v->req_vq, (uint16_t)d2);
        spin_unlock(&v->lock);
        return -1;
    }

    /* desc[0] = cmd header — always device-readable. */
    v->req_vq.desc[d0].addr_lo = (uint32_t)(uintptr_t)&v->cmd;
    v->req_vq.desc[d0].addr_hi = 0;
    v->req_vq.desc[d0].len     = sizeof(v->cmd);
    v->req_vq.desc[d0].flags   = VIRTQ_DESC_F_NEXT;
    v->req_vq.desc[d0].next    = (uint16_t)d1;

    if (data_len == 0) {
        /* No data phase. desc[1] = resp (device-writable, end). */
        v->req_vq.desc[d1].addr_lo = (uint32_t)(uintptr_t)&v->resp;
        v->req_vq.desc[d1].addr_hi = 0;
        v->req_vq.desc[d1].len     = sizeof(v->resp);
        v->req_vq.desc[d1].flags   = VIRTQ_DESC_F_WRITE;
        v->req_vq.desc[d1].next    = 0;
    } else if (data_in) {
        /* READ: [cmd READ] [resp WRITE] [data WRITE]. */
        v->req_vq.desc[d1].addr_lo = (uint32_t)(uintptr_t)&v->resp;
        v->req_vq.desc[d1].addr_hi = 0;
        v->req_vq.desc[d1].len     = sizeof(v->resp);
        v->req_vq.desc[d1].flags   = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
        v->req_vq.desc[d1].next    = (uint16_t)d2;

        v->req_vq.desc[d2].addr_lo = (uint32_t)(uintptr_t)data_buf;
        v->req_vq.desc[d2].addr_hi = 0;
        v->req_vq.desc[d2].len     = data_len;
        v->req_vq.desc[d2].flags   = VIRTQ_DESC_F_WRITE;
        v->req_vq.desc[d2].next    = 0;
    } else {
        /* WRITE: [cmd READ] [data READ] [resp WRITE]. */
        v->req_vq.desc[d1].addr_lo = (uint32_t)(uintptr_t)data_buf;
        v->req_vq.desc[d1].addr_hi = 0;
        v->req_vq.desc[d1].len     = data_len;
        v->req_vq.desc[d1].flags   = VIRTQ_DESC_F_NEXT;
        v->req_vq.desc[d1].next    = (uint16_t)d2;

        v->req_vq.desc[d2].addr_lo = (uint32_t)(uintptr_t)&v->resp;
        v->req_vq.desc[d2].addr_hi = 0;
        v->req_vq.desc[d2].len     = sizeof(v->resp);
        v->req_vq.desc[d2].flags   = VIRTQ_DESC_F_WRITE;
        v->req_vq.desc[d2].next    = 0;
    }

    virtio_submit(v->io, &v->req_vq, (uint16_t)d0);

    int rc = virtio_wait_used(v->io, &v->req_vq, 5000);
    spin_unlock(&v->lock);
    if (rc != 0) {
        kprintf("virtio-scsi: command timed out (cdb[0]=0x%x)\n", cdb[0]);
        return -1;
    }
    if (v->resp.response != VSCSI_RESPONSE_OK ||
        v->resp.status   != VSCSI_STATUS_GOOD)
    {
        kprintf("virtio-scsi: cdb[0]=0x%x response=%u status=%u\n",
                cdb[0], v->resp.response, v->resp.status);
        return -1;
    }
    return 0;
}

/* SCSI INQUIRY (opcode 0x12). 36 bytes returned. */
static int scsi_inquiry(struct vscsi *v, uint8_t out[36]) {
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    return vscsi_command(v, cdb, 6, out, 36, 1);
}

/* SCSI TEST UNIT READY (opcode 0x00). */
static int scsi_tur(struct vscsi *v) {
    uint8_t cdb[6] = { 0, 0, 0, 0, 0, 0 };
    return vscsi_command(v, cdb, 6, 0, 0, 0);
}

/* SCSI READ CAPACITY(10): opcode 0x25, returns 8 bytes:
 *   [0..3] big-endian last LBA
 *   [4..7] big-endian block size */
static int scsi_read_capacity10(struct vscsi *v,
                                uint32_t *out_last_lba,
                                uint32_t *out_block_size)
{
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t buf[8];
    int rc = vscsi_command(v, cdb, 10, buf, 8, 1);
    if (rc != 0) return rc;
    *out_last_lba   = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                    | ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
    *out_block_size = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16)
                    | ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];
    return 0;
}

/* SCSI READ(10): opcode 0x28, 4-byte BE LBA at [2..5], 2-byte BE
 * transfer length at [7..8]. */
static int scsi_read10(struct vscsi *v, uint32_t lba, uint16_t n, void *buf) {
    uint8_t cdb[10] = {
        0x28, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >>  8), (uint8_t)(lba),
        0,
        (uint8_t)(n >> 8), (uint8_t)(n),
        0
    };
    return vscsi_command(v, cdb, 10, buf, (uint32_t)n * v->block_size, 1);
}

/* SCSI WRITE(10): opcode 0x2A, same layout as READ(10). */
static int scsi_write10(struct vscsi *v, uint32_t lba, uint16_t n, const void *buf) {
    uint8_t cdb[10] = {
        0x2A, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >>  8), (uint8_t)(lba),
        0,
        (uint8_t)(n >> 8), (uint8_t)(n),
        0
    };
    return vscsi_command(v, cdb, 10, (void *)buf, (uint32_t)n * v->block_size, 0);
}

/* ---- blkdev adapter -------------------------------------------- */

static int vscsi_blkdev_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf) {
    struct vscsi *v = (struct vscsi *)d->driver_data;
    if (n == 0) return 0;
    if (n > 0xFFFF) return -1;
    return scsi_read10(v, lba, (uint16_t)n, buf);
}
static int vscsi_blkdev_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf) {
    struct vscsi *v = (struct vscsi *)d->driver_data;
    if (n == 0) return 0;
    if (n > 0xFFFF) return -1;
    return scsi_write10(v, lba, (uint16_t)n, buf);
}

/* ---- init ------------------------------------------------------- */

void virtio_scsi_init(void) {
    struct vscsi *v = &g_vscsi;
    if (v->in_use) return;

    if (pci_find(VIRTIO_VENDOR_ID, VIRTIO_LEGACY_SCSI, &v->pci) != 0) {
        return;
    }
    v->io = v->pci.io_base;
    spin_lock_init(&v->lock);

    kprintf("virtio-scsi: PCI %u:%u.%u  io=0x%x  irq=%u\n",
            v->pci.bus, v->pci.device, v->pci.func,
            (unsigned)v->io, v->pci.irq_line);

    virtio_status_reset(v->io);
    virtio_status_ack(v->io);
    virtio_status_driver(v->io);
    /* No interesting feature bits to negotiate for our minimal use. */
    virtio_negotiate(v->io, 0);

    /* Three virtqueues: control (0), event (1), request (2). We
     * only USE the request queue but virtio requires all three to be
     * configured before DRIVER_OK. */
    if (virtio_queue_init(v->io, 0, &v->ctrl_vq)  != 0 ||
        virtio_queue_init(v->io, 1, &v->event_vq) != 0 ||
        virtio_queue_init(v->io, 2, &v->req_vq)   != 0)
    {
        kprintf("virtio-scsi: queue setup failed\n");
        virtio_status_failed(v->io);
        return;
    }
    virtio_install_irq(v->io, v->pci.irq_line, 0, v);
    virtio_status_driver_ok(v->io);

    /* SAM LUN encoding for the first target: lun[0] = 1 (address
     * method "logical unit"), lun[1] = target id = 0, lun[2..7] = 0.
     * QEMU's virtio-scsi rejects lun[0] != 1 with response = BAD_TARGET. */
    for (int i = 0; i < 8; i++) v->lun[i] = 0;
    v->lun[0] = 1;
    /* Block size is filled by READ CAPACITY, but vscsi_command needs
     * it set on the way IN (the data length for READ/WRITE is
     * n_blocks * block_size). Provide a safe default for the probe
     * sequence — INQUIRY uses its own length, READ CAPACITY uses 8
     * bytes explicitly, neither relies on this. */
    v->block_size = 512;

    /* TEST UNIT READY can NAK while removable media spins up — the
     * QEMU scsi-hd backing is ready immediately but try a few times
     * for real-hardware-pattern parity. */
    int rc = -1;
    for (int retries = 0; retries < 5; retries++) {
        rc = scsi_tur(v);
        if (rc == 0) break;
    }

    uint8_t inq[36];
    if (scsi_inquiry(v, inq) != 0) {
        kprintf("virtio-scsi: INQUIRY failed — no target?\n");
        return;
    }
    /* Vendor (8 bytes) + product (16 bytes) at offsets 8 + 16. */
    char vendor[9], product[17];
    for (int i = 0; i < 8;  i++) vendor[i]  = (char)inq[8 + i];
    for (int i = 0; i < 16; i++) product[i] = (char)inq[16 + i];
    vendor[8] = product[16] = 0;
    for (int i = 7;  i >= 0 && vendor[i]  == ' '; i--) vendor[i]  = 0;
    for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;

    uint32_t last_lba = 0, block_size = 0;
    if (scsi_read_capacity10(v, &last_lba, &block_size) != 0) {
        kprintf("virtio-scsi: READ CAPACITY failed\n");
        return;
    }
    v->n_blocks   = last_lba + 1;
    v->block_size = block_size;

    kprintf("virtio-scsi: vendor=\"%s\"  product=\"%s\"\n", vendor, product);
    kprintf("virtio-scsi: capacity = %u blocks * %u B = %u KiB\n",
            (unsigned)v->n_blocks, (unsigned)v->block_size,
            (unsigned)((v->n_blocks * (uint64_t)v->block_size) / 1024));

    /* Register as blkdev. */
    v->bdev.block_size  = v->block_size;
    v->bdev.n_blocks    = v->n_blocks;
    v->bdev.read        = vscsi_blkdev_read;
    v->bdev.write       = vscsi_blkdev_write;
    v->bdev.driver_data = v;
    const char *nm = "vscsi0";
    int i;
    for (i = 0; nm[i] && i < BLKDEV_NAME_LEN - 1; i++) v->bdev.name[i] = nm[i];
    v->bdev.name[i] = 0;

    int slot = blkdev_register(&v->bdev);
    if (slot < 0) {
        kprintf("virtio-scsi: blkdev table full\n");
        return;
    }
    v->in_use = 1;
    kprintf("virtio-scsi: registered as blkdev slot %d (%s)\n",
            slot, v->bdev.name);

    /* Sanity probe: write a known pattern to the last sector, read it
     * back, restore. Confirms WRITE(10) + READ(10) round-trip works
     * end-to-end before any FS layer / bcache caller depends on it. */
    static uint8_t saved[512];
    static uint8_t pattern[512];
    static uint8_t readback[512];
    uint32_t probe_lba = v->n_blocks - 1;
    if (vscsi_blkdev_read(&v->bdev, probe_lba, 1, saved) != 0) {
        kprintf("virtio-scsi: %s: probe READ failed\n", v->bdev.name);
        return;
    }
    for (int j = 0; j < 512; j++) pattern[j] = (uint8_t)(j ^ 0x5A);
    if (vscsi_blkdev_write(&v->bdev, probe_lba, 1, pattern) != 0) {
        kprintf("virtio-scsi: %s: probe WRITE failed\n", v->bdev.name);
        return;
    }
    if (vscsi_blkdev_read(&v->bdev, probe_lba, 1, readback) != 0) {
        kprintf("virtio-scsi: %s: probe READBACK failed\n", v->bdev.name);
        return;
    }
    int match = 1;
    for (int j = 0; j < 512; j++) {
        if (readback[j] != pattern[j]) { match = 0; break; }
    }
    if (!match) {
        kprintf("virtio-scsi: %s: probe MISMATCH (round-trip broken)\n",
                v->bdev.name);
        return;
    }
    vscsi_blkdev_write(&v->bdev, probe_lba, 1, saved);
    kprintf("virtio-scsi: %s: probe ok (write+read round-trip @ sector %u)\n",
            v->bdev.name, (unsigned)probe_lba);
}
