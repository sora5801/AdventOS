/*
 * NVMe driver. See nvme.h.
 *
 * Register map (MMIO at BAR0):
 *
 *   0x00 CAP   (8 B)  Controller capabilities
 *                       [0..15]  MQES   max queue entries supported - 1
 *                       [32..35] DSTRD  doorbell stride: bytes = 4 << DSTRD
 *                       [37..44] CSS    command set support
 *   0x08 VS    (4 B)  Version
 *   0x0C INTMS / 0x10 INTMC   IRQ mask set / clear
 *   0x14 CC    (4 B)  Controller config
 *                       [0]      EN
 *                       [4..6]   CSS
 *                       [11..13] MPS (memory page size: log2 - 12)
 *                       [14..15] AMS (arbitration mechanism, 0=RR)
 *                       [20..23] IOCQES (log2 of CQ entry size, 4 = 16 B)
 *                       [16..19] IOSQES (log2 of SQ entry size, 6 = 64 B)
 *                       [24..27] SHN (shutdown notification)
 *   0x1C CSTS  (4 B)  Controller status
 *                       [0]      RDY
 *                       [1]      CFS
 *                       [2..3]   SHST
 *   0x24 AQA   (4 B)  Admin queue attributes
 *                       [0..11]  ASQS  admin SQ size (entries - 1)
 *                       [16..27] ACQS  admin CQ size (entries - 1)
 *   0x28 ASQ   (8 B)  Admin SQ base (low 32 + high 32; we use low only)
 *   0x30 ACQ   (8 B)  Admin CQ base
 *
 *   0x1000 + (2y    )*DSTRD = SQ y tail doorbell  (write to kick host)
 *   0x1000 + (2y + 1)*DSTRD = CQ y head doorbell  (write after consuming)
 *
 *   y = 0 is the admin pair. y = 1 is our first I/O pair.
 *
 * SQ entry (64 bytes): see struct nvme_sq_entry below.
 * CQ entry (16 bytes): see struct nvme_cq_entry. Phase bit flips on
 * every wrap so the host knows which entries are valid.
 *
 * Bring-up:
 *   1. CC.EN = 0; wait CSTS.RDY = 0.
 *   2. Allocate admin SQ (4 KiB) + admin CQ (1 KiB).
 *   3. Write AQA, ASQ, ACQ.
 *   4. Write CC with IOSQES=6 + IOCQES=4 + CSS=0 + EN=1.
 *   5. Wait CSTS.RDY = 1.
 *   6. Admin: IDENTIFY (CNS=1) controller; CNS=0 namespace.
 *   7. Admin: CREATE_IO_CQ (qid=1, IEN=1); CREATE_IO_SQ (qid=1, cqid=1).
 *
 * Each I/O command:
 *   - Write to SQ[tail], bump tail, write SQ tail doorbell.
 *   - Wait for CQ[head] to have the expected phase bit + CID.
 *   - Bump CQ head, write CQ head doorbell.
 *   - Return based on status bits.
 */
#include "nvme.h"
#include "pci.h"
#include "blkdev.h"
#include "kmalloc.h"
#include "paging.h"
#include "kprintf.h"
#include "string.h"
#include "spinlock.h"
#include "pit.h"
#include "isr.h"
#include "pic.h"

#define NVME_VENDOR        0x1B36
#define NVME_DEV_QEMU      0x0010    /* QEMU's NVMe controller */

/* Controller register offsets. */
#define CTRL_CAP_LO   0x00
#define CTRL_CAP_HI   0x04
#define CTRL_VS       0x08
#define CTRL_INTMS    0x0C
#define CTRL_INTMC    0x10
#define CTRL_CC       0x14
#define CTRL_CSTS     0x1C
#define CTRL_AQA      0x24
#define CTRL_ASQ_LO   0x28
#define CTRL_ASQ_HI   0x2C
#define CTRL_ACQ_LO   0x30
#define CTRL_ACQ_HI   0x34

/* CC bits. */
#define CC_EN         (1u << 0)
#define CC_CSS_NVM    (0u << 4)
#define CC_MPS_4K     (0u << 7)    /* MPS=0 -> 2^(12+0) = 4 KiB pages */
#define CC_IOSQES_64  (6u << 16)   /* log2(64) = 6 */
#define CC_IOCQES_16  (4u << 20)   /* log2(16) = 4 */

#define CSTS_RDY      (1u << 0)
#define CSTS_CFS      (1u << 1)

/* Admin / I/O opcodes. */
#define NVME_ADM_DELETE_IO_SQ  0x00
#define NVME_ADM_CREATE_IO_SQ  0x01
#define NVME_ADM_DELETE_IO_CQ  0x04
#define NVME_ADM_CREATE_IO_CQ  0x05
#define NVME_ADM_IDENTIFY      0x06

#define NVME_IO_WRITE          0x01
#define NVME_IO_READ           0x02
#define NVME_IO_FLUSH          0x00

#define ADMIN_Q_DEPTH  16     /* 16 admin entries — plenty for setup */
#define IO_Q_DEPTH     32     /* 32 I/O entries; we use one at a time */
#define NVME_MAX_LBA_PER_CMD  16  /* PRP1 + PRP2 = 2 pages = 8 KiB / 512 = 16 */

/* On-the-wire structures. */

struct nvme_sq_entry {              /* 64 bytes */
    uint32_t cdw0;                  /* opcode | flags<<8 | cid<<16 */
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint32_t mptr_lo;
    uint32_t mptr_hi;
    uint32_t prp1_lo;
    uint32_t prp1_hi;
    uint32_t prp2_lo;
    uint32_t prp2_hi;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

struct nvme_cq_entry {              /* 16 bytes */
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;                /* bit 0 = phase, bits 1..15 = status */
} __attribute__((packed));

/* Identify-controller relevant fields (out of 4096 bytes). */
struct nvme_id_ctrl {
    uint16_t vid;
    uint16_t ssvid;
    char     sn[20];
    char     mn[40];
    char     fr[8];
    uint8_t  filler[4096 - 20 - 40 - 8 - 4];
} __attribute__((packed));

/* Per-controller state. */
struct nvme_ctrl {
    int                in_use;
    struct pci_device  pci;
    volatile uint8_t  *regs;          /* BAR0 MMIO base */
    uint32_t           doorbell_stride;  /* bytes per doorbell */

    /* Admin queue pair. */
    struct nvme_sq_entry *adm_sq;
    struct nvme_cq_entry *adm_cq;
    uint16_t              adm_sq_tail;
    uint16_t              adm_cq_head;
    uint8_t               adm_cq_phase;  /* expected phase bit */
    uint16_t              next_adm_cid;

    /* I/O queue pair (qid = 1). */
    struct nvme_sq_entry *io_sq;
    struct nvme_cq_entry *io_cq;
    uint16_t              io_sq_tail;
    uint16_t              io_cq_head;
    uint8_t               io_cq_phase;
    uint16_t              next_io_cid;

    /* Namespace info (NSID=1 only for now). */
    uint64_t              ns_size_lbas;    /* total LBAs in the namespace */
    uint32_t              lba_bytes;        /* sector size in bytes */

    spinlock_t            lock;
    volatile uint32_t     io_completed;     /* set by IRQ — bit per outstanding CID */
    struct blkdev         bdev;
};

static struct nvme_ctrl g_nvme;

/* ---- MMIO helpers ---------------------------------------------- */

static inline uint32_t cr32(uint32_t off) {
    return *(volatile uint32_t *)(g_nvme.regs + off);
}
static inline void cw32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_nvme.regs + off) = v;
}

static inline void doorbell_sq(int qid, uint16_t tail) {
    uint32_t off = 0x1000 + (uint32_t)(2 * qid) * g_nvme.doorbell_stride;
    cw32(off, tail);
}
static inline void doorbell_cq(int qid, uint16_t head) {
    uint32_t off = 0x1000 + (uint32_t)(2 * qid + 1) * g_nvme.doorbell_stride;
    cw32(off, head);
}

/* ---- IRQ handler ----------------------------------------------- */

/* Walk the I/O CQ. Each entry we find with the expected phase bit
 * was just completed by the device. Set the matching bit in
 * io_completed so wait_io_cqe sees it, advance the head pointer,
 * flip the expected phase if we wrapped. */
static void drain_io_cq(struct nvme_ctrl *n) {
    for (;;) {
        struct nvme_cq_entry *e = &n->io_cq[n->io_cq_head];
        uint16_t status = e->status;
        uint16_t phase  = status & 1;
        if (phase != n->io_cq_phase) return;
        /* Record this completion. status bits 1..15 = error code; 0 = ok. */
        n->io_completed |= (1u << e->cid);
        n->io_cq_head = (uint16_t)((n->io_cq_head + 1) % IO_Q_DEPTH);
        if (n->io_cq_head == 0) n->io_cq_phase ^= 1;
        doorbell_cq(1, n->io_cq_head);
    }
}

static void nvme_irq(struct registers *r) {
    (void)r;
    if (!g_nvme.in_use) return;
    drain_io_cq(&g_nvme);
}

/* ---- queue allocation ----------------------------------------- */

/* Allocate a page-aligned, zero-filled buffer of `pages` × 4 KiB. */
static void *alloc_pages(int pages) {
    /* kmalloc returns 16-byte aligned; oversize and round up to a
     * page. */
    uint8_t *raw = (uint8_t *)kmalloc(pages * 4096 + 4096);
    if (!raw) return 0;
    uintptr_t a = ((uintptr_t)raw + 0xFFFu) & ~(uintptr_t)0xFFFu;
    void *page = (void *)a;
    memset(page, 0, pages * 4096);
    return page;
}

/* ---- admin command submission --------------------------------- */

/* Push one admin entry, ring the doorbell, poll the admin CQ for
 * completion. Returns 0 + writes optional cdw0 result on success, -1
 * with kprintf on failure. */
static int admin_submit(struct nvme_ctrl *n,
                        const struct nvme_sq_entry *cmd,
                        uint32_t *out_result)
{
    spin_lock(&n->lock);

    /* Inject the CID we'll match in CQ. */
    uint16_t cid = n->next_adm_cid++;
    struct nvme_sq_entry e = *cmd;
    e.cdw0 = (e.cdw0 & 0xFFFFu) | ((uint32_t)cid << 16);

    n->adm_sq[n->adm_sq_tail] = e;
    n->adm_sq_tail = (uint16_t)((n->adm_sq_tail + 1) % ADMIN_Q_DEPTH);
    doorbell_sq(0, n->adm_sq_tail);

    /* Poll the admin CQ — the admin queue is small and only used at
     * boot so a busy-wait with a generous deadline is fine.
     * Deadline: ~5 s by tick count. */
    uint32_t start = pit_ticks();
    uint32_t status = 0;
    for (;;) {
        struct nvme_cq_entry *cqe = &n->adm_cq[n->adm_cq_head];
        uint16_t s = cqe->status;
        if ((s & 1) == n->adm_cq_phase) {
            status = s;
            if (out_result) *out_result = cqe->result;
            n->adm_cq_head = (uint16_t)((n->adm_cq_head + 1) % ADMIN_Q_DEPTH);
            if (n->adm_cq_head == 0) n->adm_cq_phase ^= 1;
            doorbell_cq(0, n->adm_cq_head);
            break;
        }
        if (pit_ticks() - start > 500) {
            kprintf("nvme: admin timeout (opcode=0x%x)\n", cmd->cdw0 & 0xFFu);
            spin_unlock(&n->lock);
            return -1;
        }
    }
    spin_unlock(&n->lock);

    /* Status code = bits 1..15. 0 means success. */
    uint16_t sc = (status >> 1) & 0x7FFFu;
    if (sc != 0) {
        kprintf("nvme: admin opcode=0x%x status=0x%x\n",
                cmd->cdw0 & 0xFFu, sc);
        return -1;
    }
    return 0;
}

/* ---- I/O command submission ----------------------------------- */

static int io_submit(struct nvme_ctrl *n,
                     uint8_t opcode,
                     uint64_t slba,
                     uint16_t nlb_minus1,
                     void *buf,
                     uint32_t bytes,
                     uint32_t prp2_phys)
{
    spin_lock(&n->lock);

    uint16_t cid = (uint16_t)(n->next_io_cid++ & 0x1Fu);  /* keep < 32 */
    n->io_completed &= ~(1u << cid);

    struct nvme_sq_entry e;
    memset(&e, 0, sizeof(e));
    e.cdw0   = (uint32_t)opcode | ((uint32_t)cid << 16);
    e.nsid   = 1;
    e.prp1_lo = (uint32_t)(uintptr_t)buf;
    e.prp1_hi = 0;
    e.prp2_lo = prp2_phys;
    e.prp2_hi = 0;
    e.cdw10  = (uint32_t)(slba       );
    e.cdw11  = (uint32_t)(slba >> 32 );
    e.cdw12  = nlb_minus1;
    (void)bytes;

    n->io_sq[n->io_sq_tail] = e;
    n->io_sq_tail = (uint16_t)((n->io_sq_tail + 1) % IO_Q_DEPTH);
    doorbell_sq(1, n->io_sq_tail);
    spin_unlock(&n->lock);

    /* Wait for IRQ to mark our CID complete. Mirror virtio_wait_used
     * pattern: pushfl/sti/hlt around our state check so the wait
     * doesn't burn CPU. */
    uint32_t start = pit_ticks();
    while (!(n->io_completed & (1u << cid))) {
        __asm__ volatile (
            "pushfl\n\t"
            "sti\n\t"
            "hlt\n\t"
            "popfl\n\t"
            ::: "memory", "cc"
        );
        /* Backup: peek the CQ in case the IRQ landed on a sibling
         * line that doesn't know about us (chain dispatch handles
         * this in principle, but cheap to double-check). */
        drain_io_cq(n);
        if (pit_ticks() - start > 500) {
            kprintf("nvme: I/O timeout (opcode=0x%x, slba=%u, nlb=%u)\n",
                    opcode, (uint32_t)slba, nlb_minus1 + 1);
            return -1;
        }
    }
    n->io_completed &= ~(1u << cid);
    return 0;
}

/* ---- blkdev adapter -------------------------------------------- */

/* Build the PRP2 value for a transfer of `bytes` starting at `buf`.
 * Returns the PRP2 physical pointer. For tiny transfers that fit in
 * one page, PRP2 = 0. For 4-KiB-boundary-crossing transfers up to
 * 2 pages, PRP2 = phys of the second page. Larger than 2 pages would
 * need a PRP list page — for that we'd allocate one and chain. We
 * cap at 16 LBAs (= 8 KiB) above to dodge that. */
static uint32_t build_prp2(void *buf, uint32_t bytes) {
    uintptr_t p1 = (uintptr_t)buf;
    /* Bytes spilling past the page containing PRP1. */
    uint32_t in_page = 4096u - (uint32_t)(p1 & 0xFFFu);
    if (bytes <= in_page) return 0;
    return (uint32_t)((p1 & ~0xFFFu) + 4096u);
}

static int nvme_blkdev_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf) {
    struct nvme_ctrl *c = (struct nvme_ctrl *)d->driver_data;
    if (n == 0) return 0;
    /* Split into 16-LBA chunks so each command stays inside PRP1+PRP2. */
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        uint32_t chunk = n > NVME_MAX_LBA_PER_CMD ? NVME_MAX_LBA_PER_CMD : n;
        uint32_t bytes = chunk * c->lba_bytes;
        uint32_t prp2  = build_prp2(p, bytes);
        if (io_submit(c, NVME_IO_READ, lba, (uint16_t)(chunk - 1),
                      p, bytes, prp2) != 0) return -1;
        p   += bytes;
        lba += chunk;
        n   -= chunk;
    }
    return 0;
}

static int nvme_blkdev_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf) {
    struct nvme_ctrl *c = (struct nvme_ctrl *)d->driver_data;
    if (n == 0) return 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        uint32_t chunk = n > NVME_MAX_LBA_PER_CMD ? NVME_MAX_LBA_PER_CMD : n;
        uint32_t bytes = chunk * c->lba_bytes;
        uint32_t prp2  = build_prp2((void *)p, bytes);
        if (io_submit(c, NVME_IO_WRITE, lba, (uint16_t)(chunk - 1),
                      (void *)p, bytes, prp2) != 0) return -1;
        p   += bytes;
        lba += chunk;
        n   -= chunk;
    }
    return 0;
}

/* ---- init ------------------------------------------------------ */

/* Map `n_pages` pages of MMIO starting at phys. Returns the virt
 * pointer (we identity-map). */
static void *map_mmio(uintptr_t phys, int n_pages) {
    for (int i = 0; i < n_pages; i++) {
        if (paging_map(phys + i * 4096u, phys + i * 4096u,
                       PTE_PRESENT | PTE_WRITABLE) < 0) {
            return 0;
        }
    }
    return (void *)phys;
}

void nvme_init(void) {
    struct nvme_ctrl *n = &g_nvme;
    if (n->in_use) return;

    if (pci_find(NVME_VENDOR, NVME_DEV_QEMU, &n->pci) != 0) {
        return;      /* not present — silent */
    }

    /* Enable bus master + MMIO decode. */
    uint16_t cmd = (uint16_t)pci_config_read16(n->pci.bus, n->pci.device,
                                               n->pci.func, PCI_CFG_COMMAND);
    pci_config_write32(n->pci.bus, n->pci.device, n->pci.func,
                       PCI_CFG_COMMAND, cmd | 0x6u);   /* MEM + BM */

    /* BAR0 is 64-bit MMIO. Lower dword has phys[31..2] in bits 31..4.
     * We only need bits 31..14 (16 KiB BAR). */
    uint32_t bar0 = pci_config_read32(n->pci.bus, n->pci.device,
                                      n->pci.func, PCI_CFG_BAR0);
    if (bar0 & 1u) {
        kprintf("nvme: BAR0 is I/O space (0x%x), expected MMIO\n", bar0);
        return;
    }
    uintptr_t mmio = (uintptr_t)(bar0 & ~0xFu);
    n->regs = (volatile uint8_t *)map_mmio(mmio, 4);   /* 16 KiB */
    if (!n->regs) {
        kprintf("nvme: paging_map(0x%x) failed\n", (unsigned)mmio);
        return;
    }

    kprintf("nvme: PCI %u:%u.%u  bar0=0x%x  irq=%u\n",
            n->pci.bus, n->pci.device, n->pci.func,
            bar0, n->pci.irq_line);

    /* Read CAP — only the low 32 bits matter for our use (MQES, DSTRD). */
    uint32_t cap_lo = cr32(CTRL_CAP_LO);
    uint32_t mqes   = (cap_lo & 0xFFFFu) + 1;
    uint32_t dstrd_log = (cap_lo >> 0) & 0u;   /* placeholder */
    /* DSTRD is in bits 35:32 of the full 64-bit CAP — i.e. bits 3:0 of the high dword. */
    uint32_t cap_hi = cr32(CTRL_CAP_HI);
    dstrd_log = cap_hi & 0xFu;
    n->doorbell_stride = 4u << dstrd_log;

    kprintf("nvme: CAP_LO=0x%x  MQES=%u  doorbell_stride=%u\n",
            cap_lo, mqes, n->doorbell_stride);

    /* Disable controller. */
    cw32(CTRL_CC, 0);
    /* Wait CSTS.RDY -> 0. */
    {
        uint32_t start = pit_ticks();
        while (cr32(CTRL_CSTS) & CSTS_RDY) {
            if (pit_ticks() - start > 500) {
                kprintf("nvme: controller stuck enabled\n");
                return;
            }
        }
    }

    /* Allocate admin queues. */
    n->adm_sq = alloc_pages(1);    /* 1 page = 4 KiB; 64 entries fit */
    n->adm_cq = alloc_pages(1);    /* 1 page = ample for 16 16-B entries */
    if (!n->adm_sq || !n->adm_cq) {
        kprintf("nvme: admin queue alloc failed\n");
        return;
    }
    n->adm_sq_tail  = 0;
    n->adm_cq_head  = 0;
    n->adm_cq_phase = 1;             /* CQE phase starts at 1, flips on wrap */
    n->next_adm_cid = 0;

    /* AQA: ASQS bits 11:0, ACQS bits 27:16. Sizes are entries - 1. */
    cw32(CTRL_AQA, ((ADMIN_Q_DEPTH - 1) & 0xFFFu) |
                   (((ADMIN_Q_DEPTH - 1) & 0xFFFu) << 16));
    cw32(CTRL_ASQ_LO, (uint32_t)(uintptr_t)n->adm_sq);
    cw32(CTRL_ASQ_HI, 0);
    cw32(CTRL_ACQ_LO, (uint32_t)(uintptr_t)n->adm_cq);
    cw32(CTRL_ACQ_HI, 0);

    /* Mask all IRQs until we're ready. */
    cw32(CTRL_INTMS, 0xFFFFFFFFu);

    /* Bring up the controller. CC.MPS=0 (4 KiB pages), IOSQES=6, IOCQES=4. */
    cw32(CTRL_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES_64 | CC_IOCQES_16);

    /* Wait CSTS.RDY -> 1. */
    {
        uint32_t start = pit_ticks();
        for (;;) {
            uint32_t csts = cr32(CTRL_CSTS);
            if (csts & CSTS_RDY) break;
            if (csts & CSTS_CFS) {
                kprintf("nvme: controller fatal status\n");
                return;
            }
            if (pit_ticks() - start > 500) {
                kprintf("nvme: controller failed to ready\n");
                return;
            }
        }
    }

    spin_lock_init(&n->lock);

    /* IDENTIFY controller — confirms it's alive + gives us the model. */
    void *id_buf = alloc_pages(1);
    if (!id_buf) { kprintf("nvme: id_buf alloc failed\n"); return; }
    {
        struct nvme_sq_entry e; memset(&e, 0, sizeof(e));
        e.cdw0    = NVME_ADM_IDENTIFY;
        e.nsid    = 0;
        e.prp1_lo = (uint32_t)(uintptr_t)id_buf;
        e.cdw10   = 1;     /* CNS = 1 (identify controller) */
        if (admin_submit(n, &e, 0) != 0) return;
    }
    {
        struct nvme_id_ctrl *ic = (struct nvme_id_ctrl *)id_buf;
        char mn[41]; for (int i = 0; i < 40; i++) mn[i] = ic->mn[i];
        mn[40] = 0;
        for (int i = 39; i >= 0 && mn[i] == ' '; i--) mn[i] = 0;
        kprintf("nvme: vid=0x%x  model=\"%s\"\n",
                (unsigned)ic->vid, mn);
    }

    /* IDENTIFY namespace 1 — get capacity + LBA format. */
    {
        struct nvme_sq_entry e; memset(&e, 0, sizeof(e));
        e.cdw0    = NVME_ADM_IDENTIFY;
        e.nsid    = 1;
        e.prp1_lo = (uint32_t)(uintptr_t)id_buf;
        e.cdw10   = 0;     /* CNS = 0 (identify namespace) */
        if (admin_submit(n, &e, 0) != 0) return;
    }
    {
        uint8_t *p = (uint8_t *)id_buf;
        uint64_t nsze = (uint64_t)p[0]
                      | ((uint64_t)p[1] << 8)
                      | ((uint64_t)p[2] << 16)
                      | ((uint64_t)p[3] << 24)
                      | ((uint64_t)p[4] << 32)
                      | ((uint64_t)p[5] << 40)
                      | ((uint64_t)p[6] << 48)
                      | ((uint64_t)p[7] << 56);
        uint8_t flbas      = p[26] & 0xFu;        /* current LBA format index */
        uint32_t lba_fmt  = ((uint32_t)p[128 + flbas * 4 + 0])
                          | ((uint32_t)p[128 + flbas * 4 + 1] << 8)
                          | ((uint32_t)p[128 + flbas * 4 + 2] << 16)
                          | ((uint32_t)p[128 + flbas * 4 + 3] << 24);
        uint8_t lbads     = (uint8_t)((lba_fmt >> 16) & 0xFFu);
        n->ns_size_lbas   = nsze;
        n->lba_bytes      = 1u << lbads;
        kprintf("nvme: namespace 1: %u LBAs * %u B = %u KiB\n",
                (unsigned)(nsze & 0xFFFFFFFFu),
                n->lba_bytes,
                (unsigned)((nsze * n->lba_bytes) / 1024));
    }

    if (n->lba_bytes != 512) {
        kprintf("nvme: unsupported LBA size %u (need 512)\n", n->lba_bytes);
        return;
    }

    /* Allocate I/O queue pair (qid=1). */
    n->io_sq = alloc_pages(1);
    n->io_cq = alloc_pages(1);
    if (!n->io_sq || !n->io_cq) {
        kprintf("nvme: I/O queue alloc failed\n");
        return;
    }
    n->io_sq_tail  = 0;
    n->io_cq_head  = 0;
    n->io_cq_phase = 1;
    n->next_io_cid = 0;
    n->io_completed = 0;

    /* Install IRQ + unmask MSI-X / INTx legacy vector 0 before
     * CREATE_IO_CQ so the device can deliver completion IRQs. */
    g_nvme.in_use = 1;       /* set early so ahci_irq pattern: handler
                              * checks in_use first. Set BEFORE
                              * isr_register_irq + clearing INTMS. */
    isr_register_irq(n->pci.irq_line, nvme_irq);
    pic_clear_mask((uint8_t)n->pci.irq_line);
    cw32(CTRL_INTMC, 1u);    /* Unmask vector 0 (our only CQ). */

    /* CREATE_IO_CQ before CREATE_IO_SQ — the SQ refers to the CQ id. */
    {
        struct nvme_sq_entry e; memset(&e, 0, sizeof(e));
        e.cdw0    = NVME_ADM_CREATE_IO_CQ;
        e.prp1_lo = (uint32_t)(uintptr_t)n->io_cq;
        e.cdw10   = ((IO_Q_DEPTH - 1) << 16) | 1;        /* size-1 + qid=1 */
        e.cdw11   = (1u << 0) | (1u << 1);                /* PC=1, IEN=1 */
        if (admin_submit(n, &e, 0) != 0) return;
    }
    {
        struct nvme_sq_entry e; memset(&e, 0, sizeof(e));
        e.cdw0    = NVME_ADM_CREATE_IO_SQ;
        e.prp1_lo = (uint32_t)(uintptr_t)n->io_sq;
        e.cdw10   = ((IO_Q_DEPTH - 1) << 16) | 1;        /* size-1 + qid=1 */
        e.cdw11   = (1u << 0) | (1u << 16);               /* PC=1, CQID=1 */
        if (admin_submit(n, &e, 0) != 0) return;
    }

    /* Register as blkdev. */
    uint32_t n_blocks = (uint32_t)(n->ns_size_lbas > 0xFFFFFFFFu ?
                                   0xFFFFFFFFu : n->ns_size_lbas);
    n->bdev.block_size  = 512;
    n->bdev.n_blocks    = n_blocks;
    n->bdev.read        = nvme_blkdev_read;
    n->bdev.write       = nvme_blkdev_write;
    n->bdev.driver_data = n;
    const char *nm = "nvme0";
    int i;
    for (i = 0; nm[i] && i < BLKDEV_NAME_LEN - 1; i++) n->bdev.name[i] = nm[i];
    n->bdev.name[i] = 0;

    int slot = blkdev_register(&n->bdev);
    if (slot < 0) {
        kprintf("nvme: blkdev table full\n");
        return;
    }
    kprintf("nvme: registered as blkdev slot %d (%s)\n", slot, n->bdev.name);

    /* Sanity probe: write a known pattern to the last sector, read
     * it back, restore. Same pattern as AHCI / virtio-scsi. */
    static uint8_t saved[512];
    static uint8_t pattern[512];
    static uint8_t readback[512];
    uint32_t probe_lba = n_blocks - 1;
    if (nvme_blkdev_read(&n->bdev, probe_lba, 1, saved) != 0) {
        kprintf("nvme: %s: probe READ failed\n", n->bdev.name); return;
    }
    for (int j = 0; j < 512; j++) pattern[j] = (uint8_t)(j ^ 0xC3);
    if (nvme_blkdev_write(&n->bdev, probe_lba, 1, pattern) != 0) {
        kprintf("nvme: %s: probe WRITE failed\n", n->bdev.name); return;
    }
    if (nvme_blkdev_read(&n->bdev, probe_lba, 1, readback) != 0) {
        kprintf("nvme: %s: probe READBACK failed\n", n->bdev.name); return;
    }
    int match = 1;
    for (int j = 0; j < 512; j++) {
        if (readback[j] != pattern[j]) { match = 0; break; }
    }
    if (!match) {
        kprintf("nvme: %s: probe MISMATCH\n", n->bdev.name);
        return;
    }
    nvme_blkdev_write(&n->bdev, probe_lba, 1, saved);
    kprintf("nvme: %s: probe ok (write+read round-trip @ sector %u)\n",
            n->bdev.name, (unsigned)probe_lba);
}
