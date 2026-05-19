/*
 * AHCI driver. See ahci.h.
 *
 * Memory layout, 32-bit MMIO at BAR5:
 *
 *   HBA Generic Host Control (offset 0..0x9F):
 *     0x00 CAP        capabilities (RO)
 *     0x04 GHC        global host control (bit 0 HR, bit 31 AE)
 *     0x08 IS         interrupt status (per-port bitmap)
 *     0x0C PI         ports implemented (per-port bitmap)
 *     0x10 VS         AHCI version
 *
 *   Port registers (one block per port, 0x100 + port*0x80 .. +0x7F):
 *     0x00 CLB / 0x04 CLBU    command-list base (1 KiB aligned)
 *     0x08 FB  / 0x0C FBU     FIS receive-area base (256 B aligned)
 *     0x10 IS / 0x14 IE       per-port interrupt status / enable
 *     0x18 CMD                command (bit 0 ST, bit 4 FRE, etc.)
 *     0x20 TFD                task-file data (status + error)
 *     0x24 SIG                device signature (0x101 = SATA disk)
 *     0x28 SSTS               SATA status (DET bits 0..3)
 *     0x2C SCTL               SATA control
 *     0x30 SERR               SATA error
 *     0x38 CI                 commands issued (bitmap of slots in flight)
 *
 *   Command list (1 KiB, 32 headers of 32 bytes each, port-local):
 *     header.flags16 = (CFL=5) | (W=0/1 for read/write)
 *     header.prdtl   = number of PRD entries (1 for our short reads)
 *     header.ctba    = command-table phys base (128 B aligned)
 *
 *   Command table (256 bytes — one CFIS + 8 PRDs is plenty):
 *     0x00 CFIS  (64 B)   command FIS, e.g. H2D Register FIS
 *     0x40 ACMD  (16 B)   ATAPI command (unused)
 *     0x50 rsv   (48 B)
 *     0x80 PRDT  (16 B per entry)
 *
 *   PRD entry:
 *     0x00 DBA / 0x04 DBAU  buffer physical address
 *     0x08 rsv
 *     0x0C DBC (low 22 bits = byte count - 1) | I (bit 31 = irq)
 *
 *   H2D Register FIS (20 bytes, padded into the 64 B CFIS):
 *     [0]  fis_type = 0x27
 *     [1]  pmp_c    = 0x80 (C=1, command)
 *     [2]  command  (ATA opcode, e.g. 0x25 = READ DMA EXT)
 *     [3]  features-low
 *     [4..6] LBA[0..23]
 *     [7]  device (bit 6 = LBA mode)
 *     [8..10] LBA[24..47]
 *     [11] features-high
 *     [12..13] count
 *     [14] icc
 *     [15] control
 *     [16..19] aux
 *
 * Init sequence (per port that has a SATA disk):
 *   1. CMD.ST = 0; wait CMD.CR = 0.
 *   2. CMD.FRE = 0; wait CMD.FR = 0.
 *   3. Allocate command list (1 KiB) + FIS RX (256 B) + command
 *      table (256 B). All from kmalloc — identity-mapped, phys=virt.
 *   4. Write CLB/CLBU/FB/FBU.
 *   5. Clear SERR (write-1s-to-clear) and IS.
 *   6. CMD.FRE = 1.
 *   7. CMD.ST = 1.
 *
 * Issuing a command (slot 0 only):
 *   - Fill header.flags16 + prdtl, write header.ctba.
 *   - Fill command table: CFIS (H2D Register FIS), PRDT[0] (buffer).
 *   - CI = 1<<0 (kick).
 *   - Poll CI bit until clear (DMA complete) and TFD.BSY=0.
 */
#include "ahci.h"
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

#define AHCI_VENDOR     0x8086
#define AHCI_DEV_ICH9   0x2922      /* `-device ahci` in QEMU */

/* HBA register offsets. */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_VS    0x10

/* GHC bits. */
#define GHC_HR    (1u << 0)
#define GHC_IE    (1u << 1)
#define GHC_AE    (1u << 31)

/* Per-port register offsets (from port-block base = HBA + 0x100 + port*0x80). */
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SCTL  0x2C
#define P_SERR  0x30
#define P_SACT  0x34       /* NCQ: bit-per-tag of pending queued commands */
#define P_CI    0x38

/* CMD bits. */
#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)

/* TFD bits. */
#define TFD_ERR  (1u << 0)
#define TFD_DRQ  (1u << 3)
#define TFD_BSY  (1u << 7)

/* P_IE bits — enable individual interrupt sources. */
#define IE_DHRE  (1u << 0)    /* D2H Register FIS — non-NCQ completion */
#define IE_PSE   (1u << 1)    /* PIO Setup FIS */
#define IE_DSE   (1u << 2)    /* DMA Setup FIS */
#define IE_SDBE  (1u << 3)    /* Set Device Bits FIS — NCQ completion */
#define IE_TFEE  (1u << 30)   /* Task-File Error */
#define IE_HBFE  (1u << 29)   /* Host Bus Fatal Error */

/* Device signatures (P_SIG). */
#define SIG_SATA   0x00000101u

/* SSTS detection (DET, bits 0..3). */
#define DET_PRESENT_NOCOMM  1
#define DET_PRESENT_AND_OK  3

/* ATA commands we use. */
#define ATA_CMD_READ_DMA_EXT       0x25
#define ATA_CMD_WRITE_DMA_EXT      0x35
#define ATA_CMD_IDENTIFY           0xEC
/* NCQ (Native Command Queuing) variants. Different FIS layout:
 * sector count lives in features fields; the count byte holds the
 * NCQ TAG (bits 7:3). Completion delivers a Set Device Bits FIS
 * (clears SACT bit) instead of the usual D2H Register FIS. */
#define ATA_CMD_READ_FPDMA_QUEUED  0x60
#define ATA_CMD_WRITE_FPDMA_QUEUED 0x61

#define IS_NCQ_CMD(c)  ((c) == ATA_CMD_READ_FPDMA_QUEUED || \
                        (c) == ATA_CMD_WRITE_FPDMA_QUEUED)

/* Number of NCQ slots we can address. Hardware supports up to 32;
 * we allocate command tables for all of them so multi-slot use is
 * just a matter of how many concurrent callers exist. Today the BKL
 * makes that one — but switching to slot-0-only on a polled wait
 * was the bottleneck NCQ exists to remove. */
#define AHCI_N_SLOTS  32

/* ---- on-the-wire structures ------------------------------------ */

struct ahci_cmd_hdr {                /* 32 bytes; 32 of these per port. */
    uint16_t flags;                  /* CFL (low 5) | W (bit 6) | P (bit 7) */
    uint16_t prdtl;                  /* PRDT length */
    uint32_t prdbc;                  /* PRD byte count (HBA writes) */
    uint32_t ctba;                   /* Command Table base (low 32) */
    uint32_t ctbau;                  /* high 32 */
    uint32_t rsv[4];
} __attribute__((packed));

struct ahci_prd {                    /* 16 bytes. */
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv;
    uint32_t dbc;                    /* low 22 = byte count - 1; bit 31 = I */
} __attribute__((packed));

struct ahci_cmd_table {              /* 128 + 16*N bytes. We use N=1. */
    uint8_t  cfis[64];               /* command FIS */
    uint8_t  acmd[16];               /* ATAPI (unused) */
    uint8_t  rsv[48];
    struct ahci_prd prdt[8];         /* room for 8 PRDs */
} __attribute__((packed));

/* ---- driver state ---------------------------------------------- */

#define AHCI_MAX_PORTS  32

struct ahci_port {
    int                in_use;
    int                port_idx;     /* 0..31 */
    volatile uint8_t  *regs;         /* port register base */
    struct ahci_cmd_hdr   *clist;    /* command list (1 KiB, 32 hdrs) */
    void              *fis;          /* FIS RX area (256 B) */
    struct ahci_cmd_table *ctbls[AHCI_N_SLOTS];   /* per-slot tables */
    spinlock_t         lock;
    uint32_t           free_mask;    /* bit-per-slot, 1 = available */
    volatile uint32_t  completed_mask;  /* set by IRQ on completion */
    volatile uint32_t  error_mask;      /* set by IRQ on TFEE */
    uint32_t           n_sectors;
    struct blkdev      bdev;
};

static struct {
    int               in_use;
    struct pci_device pci;
    volatile uint8_t *abar;          /* HBA MMIO base */
    struct ahci_port  ports[AHCI_MAX_PORTS];
    int               n_attached;    /* port count actually with disks */
} g_ahci;

/* ---- MMIO helpers ---------------------------------------------- */

static inline uint32_t hr32(uint32_t off) {
    return *(volatile uint32_t *)(g_ahci.abar + off);
}
static inline void hw32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_ahci.abar + off) = v;
}
static inline uint32_t pr32(volatile uint8_t *port, uint32_t off) {
    return *(volatile uint32_t *)(port + off);
}
static inline void pw32(volatile uint8_t *port, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(port + off) = v;
}

/* ---- port init ------------------------------------------------- */

/* Allocate a buffer of `size` bytes aligned to `align`. Caller-owned,
 * never freed. */
static void *alloc_aligned(uint32_t size, uint32_t align) {
    /* kmalloc returns 16-byte aligned; oversize and round up. */
    uint8_t *raw = (uint8_t *)kmalloc(size + align);
    if (!raw) return 0;
    uintptr_t a = ((uintptr_t)raw + (align - 1)) & ~(uintptr_t)(align - 1);
    return (void *)a;
}

static int port_stop(volatile uint8_t *port) {
    uint32_t cmd = pr32(port, P_CMD);
    cmd &= ~(CMD_ST | CMD_FRE);
    pw32(port, P_CMD, cmd);
    /* Wait for CR + FR to clear. */
    for (int i = 0; i < 1000000; i++) {
        cmd = pr32(port, P_CMD);
        if (!(cmd & (CMD_CR | CMD_FR))) return 0;
    }
    return -1;
}

static void port_start(volatile uint8_t *port) {
    /* Wait for BSY/DRQ to clear before unleashing. */
    for (int i = 0; i < 1000000; i++) {
        if (!(pr32(port, P_TFD) & (TFD_BSY | TFD_DRQ))) break;
    }
    uint32_t cmd = pr32(port, P_CMD);
    cmd |= CMD_FRE;
    pw32(port, P_CMD, cmd);
    cmd |= CMD_ST;
    pw32(port, P_CMD, cmd);
}

/* IRQ handler — walks every attached port and updates completed_mask
 * for any commands that finished since the last call. Called from
 * irq_handler via the isr.c chain; co-exists with virtio + e1000 on
 * the same shared PCI line. */
static void ahci_irq(struct registers *r) {
    (void)r;
    if (!g_ahci.in_use) return;
    /* HBA_IS is a per-port bitmap — bit n set means port n raised
     * an interrupt. Read-then-clear-by-writing-the-same-bits-back
     * (write-1-to-clear). */
    uint32_t hba_is = hr32(HBA_IS);
    if (hba_is == 0) return;
    hw32(HBA_IS, hba_is);

    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(hba_is & (1u << i))) continue;
        struct ahci_port *p = &g_ahci.ports[i];
        if (!p->in_use) continue;
        /* Per-port IS: read + clear. Bit 30 (TFEE) = task-file error. */
        uint32_t pis = pr32(p->regs, P_IS);
        pw32(p->regs, P_IS, pis);
        if (pis & (1u << 30)) {
            /* Task-file error — mark every issued slot errored so
             * the waiter unblocks with -1. */
            p->error_mask |= ~p->free_mask;
        }
        /* For NCQ, completed bits clear from SACT.
         * For non-NCQ, they clear from CI.
         * A slot is "issued but no longer pending" if:
         *   (it was issued, i.e. ~free_mask) AND (not in CI) AND (not in SACT). */
        uint32_t ci    = pr32(p->regs, P_CI);
        uint32_t sact  = pr32(p->regs, P_SACT);
        uint32_t done  = (~p->free_mask) & ~ci & ~sact;
        p->completed_mask |= done;
    }
}

/* Wait until slot `slot` of port `p` completes. Returns 0 on success,
 * -1 on timeout or task-file error.
 *
 * Uses pushfl/sti/hlt/popfl so an enclosing spin_lock (which we may
 * be inside) doesn't gate forward progress: we temporarily enable
 * IF for the hlt, then restore the caller's IF state. */
static int wait_slot(struct ahci_port *p, int slot, uint32_t timeout_ms) {
    uint32_t bit = (1u << slot);
    uint32_t start = pit_ticks();
    uint32_t deadline = (timeout_ms * 100u) / 1000u;
    if (deadline == 0) deadline = 1;

    while (!(p->completed_mask & bit)) {
        if (p->error_mask & bit) {
            p->error_mask &= ~bit;
            return -1;
        }
        __asm__ volatile (
            "pushfl\n\t"
            "sti\n\t"
            "hlt\n\t"
            "popfl\n\t"
            ::: "memory", "cc"
        );
        /* Also peek the port directly in case the IRQ went to
         * another handler in the chain that doesn't know about us
         * (or fired before we got a chance to record state). */
        uint32_t ci    = pr32(p->regs, P_CI);
        uint32_t sact  = pr32(p->regs, P_SACT);
        if (!((ci | sact) & bit)) {
            p->completed_mask |= bit;
            break;
        }
        if (pit_ticks() - start > deadline) return -1;
    }
    p->completed_mask &= ~bit;
    if (pr32(p->regs, P_TFD) & TFD_ERR) return -1;
    return 0;
}

/* Allocate a free slot. Returns 0..AHCI_N_SLOTS-1 or -1 if all
 * busy. Caller must hold p->lock. */
static int alloc_slot(struct ahci_port *p) {
    for (int i = 0; i < AHCI_N_SLOTS; i++) {
        if (p->free_mask & (1u << i)) {
            p->free_mask &= ~(1u << i);
            return i;
        }
    }
    return -1;
}

/* Fill a fresh H2D Register FIS into the command table CFIS area.
 * Honors NCQ encoding when `cmd` is a queued opcode: sector count
 * lives in the features fields and the count byte holds the NCQ TAG
 * (low 5 bits shifted into [7:3]). */
static void build_h2d_fis(struct ahci_cmd_table *ct,
                          uint8_t  cmd,
                          uint64_t lba,
                          uint16_t count,
                          int      tag)
{
    uint8_t *f = ct->cfis;
    memset(f, 0, 64);
    f[0]  = 0x27;                    /* H2D Register FIS */
    f[1]  = 0x80;                    /* C = 1 (command, not control) */
    f[2]  = cmd;
    f[4]  = (uint8_t)(lba      );
    f[5]  = (uint8_t)(lba >>  8);
    f[6]  = (uint8_t)(lba >> 16);
    f[7]  = 0x40;                    /* LBA mode */
    f[8]  = (uint8_t)(lba >> 24);
    f[9]  = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    if (IS_NCQ_CMD(cmd)) {
        /* NCQ: count goes in features, tag goes in count[7:3]. */
        f[3]  = (uint8_t)(count      );    /* features-low  */
        f[11] = (uint8_t)(count >>  8);    /* features-high */
        f[12] = (uint8_t)((tag & 0x1F) << 3);
        f[13] = 0;
    } else {
        /* Plain LBA48 DMA: count in the count fields, no features. */
        f[12] = (uint8_t)(count      );
        f[13] = (uint8_t)(count >>  8);
    }
}

/* Common DMA path: pick a slot, build the chain, kick the port,
 * wait. `write_flag` controls the W bit in the command header.
 * Works for both NCQ (READ/WRITE FPDMA QUEUED) and legacy DMA EXT
 * (READ/WRITE DMA EXT, IDENTIFY) opcodes — the FIS builder and the
 * CI / SACT kicks branch on cmd. */
static int port_dma_xfer(struct ahci_port *p, uint8_t cmd, uint64_t lba,
                         uint16_t count, void *buf, uint32_t bytes,
                         int write_flag)
{
    spin_lock(&p->lock);
    int slot = alloc_slot(p);
    if (slot < 0) { spin_unlock(&p->lock); return -1; }

    struct ahci_cmd_hdr   *h  = &p->clist[slot];
    struct ahci_cmd_table *ct = p->ctbls[slot];

    h->flags  = (uint16_t)(5 | (write_flag ? (1u << 6) : 0));  /* CFL=5 + W */
    h->prdtl  = bytes ? 1 : 0;
    h->prdbc  = 0;
    h->ctba   = (uint32_t)(uintptr_t)ct;
    h->ctbau  = 0;

    build_h2d_fis(ct, cmd, lba, count, slot);

    if (bytes) {
        ct->prdt[0].dba  = (uint32_t)(uintptr_t)buf;
        ct->prdt[0].dbau = 0;
        ct->prdt[0].rsv  = 0;
        ct->prdt[0].dbc  = (bytes - 1) & 0x3FFFFFu;   /* I bit clear */
    }

    /* Don't blow away outstanding IS state — only ours. The IRQ
     * handler clears per-port IS after recording completions. */
    uint32_t bit = (1u << slot);
    if (IS_NCQ_CMD(cmd)) {
        /* NCQ: set SACT bit BEFORE CI. The HBA latches SACT into
         * its internal queue state when CI is written. */
        pw32(p->regs, P_SACT, bit);
    }
    pw32(p->regs, P_CI, bit);

    /* Release the port lock around the wait so other tasks could in
     * principle issue concurrent NCQ commands (BKL serializes today,
     * but the structure is ready). */
    spin_unlock(&p->lock);

    int rc = wait_slot(p, slot, /*timeout_ms=*/5000);

    /* Return the slot to the free pool no matter the outcome. */
    spin_lock(&p->lock);
    p->free_mask |= bit;
    spin_unlock(&p->lock);
    return rc;
}

/* ---- blkdev adapter -------------------------------------------- */

static int ahci_blkdev_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf) {
    struct ahci_port *p = (struct ahci_port *)d->driver_data;
    if (n == 0) return 0;
    if (n > 256) return -1;          /* count is 16-bit but our PRD
                                      * is one entry; keep it small. */
    return port_dma_xfer(p, ATA_CMD_READ_FPDMA_QUEUED, lba, (uint16_t)n,
                         buf, n * 512u, 0);
}

static int ahci_blkdev_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf) {
    struct ahci_port *p = (struct ahci_port *)d->driver_data;
    if (n == 0) return 0;
    if (n > 256) return -1;
    return port_dma_xfer(p, ATA_CMD_WRITE_FPDMA_QUEUED, lba, (uint16_t)n,
                         (void *)buf, n * 512u, 1);
}

/* IDENTIFY DEVICE returns a 512-byte info block. We extract the
 * 28-bit / 48-bit LBA sector count. */
static uint32_t ahci_identify(struct ahci_port *p) {
    static uint16_t identify_buf[256];
    if (port_dma_xfer(p, ATA_CMD_IDENTIFY, 0, 0, identify_buf, 512, 0) != 0) {
        return 0;
    }
    /* Words 60-61: 28-bit LBA total sector count.
     * Words 100-103: 48-bit LBA total sector count (preferred if non-zero
     * and bit 10 of word 83 is set, but we just trust 100-103 when it's
     * non-zero). */
    uint32_t lba28  = (uint32_t)identify_buf[60]
                    | ((uint32_t)identify_buf[61] << 16);
    uint64_t lba48  = (uint64_t)identify_buf[100]
                    | ((uint64_t)identify_buf[101] << 16)
                    | ((uint64_t)identify_buf[102] << 32)
                    | ((uint64_t)identify_buf[103] << 48);
    uint32_t sectors = lba48 ? (lba48 > 0xFFFFFFFFu ? 0xFFFFFFFFu :
                                (uint32_t)lba48) : lba28;
    return sectors;
}

/* ---- per-port bring-up ---------------------------------------- */

static int port_init(int idx) {
    struct ahci_port *p = &g_ahci.ports[idx];
    p->port_idx = idx;
    p->regs = g_ahci.abar + 0x100 + idx * 0x80;
    spin_lock_init(&p->lock);

    /* Skip ports without a present + comm-established SATA disk. */
    uint32_t ssts = pr32(p->regs, P_SSTS);
    uint32_t det = ssts & 0xFu;
    if (det != DET_PRESENT_AND_OK) return -1;
    uint32_t sig = pr32(p->regs, P_SIG);
    if (sig != SIG_SATA) {
        kprintf("ahci: port %d non-SATA signature 0x%x — skipping\n",
                idx, sig);
        return -1;
    }

    /* Quiesce + reconfigure. */
    if (port_stop(p->regs) != 0) {
        kprintf("ahci: port %d failed to stop\n", idx);
        return -1;
    }

    p->clist = (struct ahci_cmd_hdr *)alloc_aligned(1024, 1024);
    p->fis   = alloc_aligned(256, 256);
    if (!p->clist || !p->fis) {
        kprintf("ahci: port %d alloc failed\n", idx);
        return -1;
    }
    for (int s = 0; s < AHCI_N_SLOTS; s++) {
        p->ctbls[s] = (struct ahci_cmd_table *)alloc_aligned(
                          sizeof(struct ahci_cmd_table), 128);
        if (!p->ctbls[s]) {
            kprintf("ahci: port %d slot %d alloc failed\n", idx, s);
            return -1;
        }
        memset(p->ctbls[s], 0, sizeof(struct ahci_cmd_table));
    }
    memset(p->clist, 0, 1024);
    memset(p->fis,   0, 256);
    p->free_mask      = (AHCI_N_SLOTS == 32) ? 0xFFFFFFFFu
                                             : ((1u << AHCI_N_SLOTS) - 1);
    p->completed_mask = 0;
    p->error_mask     = 0;

    pw32(p->regs, P_CLB,  (uint32_t)(uintptr_t)p->clist);
    pw32(p->regs, P_CLBU, 0);
    pw32(p->regs, P_FB,   (uint32_t)(uintptr_t)p->fis);
    pw32(p->regs, P_FBU,  0);

    /* Clear errors and IS, then enable the IRQ sources we care about. */
    pw32(p->regs, P_SERR, 0xFFFFFFFFu);
    pw32(p->regs, P_IS,   0xFFFFFFFFu);
    pw32(p->regs, P_IE,   IE_DHRE | IE_PSE | IE_DSE | IE_SDBE |
                          IE_TFEE | IE_HBFE);

    /* Mark the port live for the IRQ handler BEFORE we issue any
     * commands — IDENTIFY + the sanity probe both go through the
     * IRQ-driven completion path. We'll clear this back to 0 below
     * if anything goes wrong, before returning. */
    p->in_use = 1;

    port_start(p->regs);

    /* Identify to learn capacity. */
    p->n_sectors = ahci_identify(p);
    if (p->n_sectors == 0) {
        kprintf("ahci: port %d IDENTIFY failed\n", idx);
        p->in_use = 0;
        return -1;
    }

    /* Register as blkdev. Name = "ahci<idx>". */
    p->bdev.block_size  = 512;
    p->bdev.n_blocks    = p->n_sectors;
    p->bdev.read        = ahci_blkdev_read;
    p->bdev.write       = ahci_blkdev_write;
    p->bdev.driver_data = p;
    const char *prefix = "ahci";
    int j = 0;
    while (prefix[j] && j < BLKDEV_NAME_LEN - 2) {
        p->bdev.name[j] = prefix[j]; j++;
    }
    p->bdev.name[j++] = (char)('0' + idx);
    p->bdev.name[j]   = 0;

    int slot = blkdev_register(&p->bdev);
    if (slot < 0) {
        kprintf("ahci: blkdev table full registering %s\n", p->bdev.name);
        p->in_use = 0;
        return -1;
    }
    g_ahci.n_attached++;

    kprintf("ahci: port %d -> blkdev slot %d (%s)  %u sectors  (%u MiB)\n",
            idx, slot, p->bdev.name,
            (unsigned)p->n_sectors,
            (unsigned)(p->n_sectors / 2048));

    /* Sanity probe: write a known pattern to the last sector, read
     * it back, restore. Confirms the WRITE + READ DMA paths work
     * end-to-end before any later bcache caller depends on them. */
    static uint8_t saved[512];
    static uint8_t pattern[512];
    static uint8_t readback[512];
    uint32_t probe_lba = p->n_sectors - 1;
    if (ahci_blkdev_read(&p->bdev, probe_lba, 1, saved) != 0) {
        kprintf("ahci: %s: probe READ failed\n", p->bdev.name);
        return 0;
    }
    for (int i = 0; i < 512; i++) pattern[i] = (uint8_t)(i ^ 0xA5);
    if (ahci_blkdev_write(&p->bdev, probe_lba, 1, pattern) != 0) {
        kprintf("ahci: %s: probe WRITE failed\n", p->bdev.name);
        return 0;
    }
    if (ahci_blkdev_read(&p->bdev, probe_lba, 1, readback) != 0) {
        kprintf("ahci: %s: probe READBACK failed\n", p->bdev.name);
        return 0;
    }
    int match = 1;
    for (int i = 0; i < 512; i++) {
        if (readback[i] != pattern[i]) { match = 0; break; }
    }
    if (!match) {
        kprintf("ahci: %s: probe MISMATCH (write/read round-trip broken)\n",
                p->bdev.name);
        return 0;
    }
    /* Restore so we don't leave a smudge on the disk. */
    ahci_blkdev_write(&p->bdev, probe_lba, 1, saved);
    kprintf("ahci: %s: probe ok (write+read round-trip @ sector %u)\n",
            p->bdev.name, (unsigned)probe_lba);
    return 0;
}

/* ---- HBA init -------------------------------------------------- */

void ahci_init(void) {
    if (g_ahci.in_use) return;

    if (pci_find(AHCI_VENDOR, AHCI_DEV_ICH9, &g_ahci.pci) != 0) {
        return;                      /* not present — silent */
    }

    /* BAR5 is the AHCI HBA MMIO. Read raw + mask flags. */
    uint32_t bar5 = pci_config_read32(g_ahci.pci.bus, g_ahci.pci.device,
                                      g_ahci.pci.func, 0x24);
    if (bar5 & 1u) {
        kprintf("ahci: BAR5 is I/O space (0x%x), expected MMIO\n", bar5);
        return;
    }
    uintptr_t mmio = (uintptr_t)(bar5 & ~0xFu);
    /* AHCI register file is small (~1 KiB) but map a whole page for
     * alignment. */
    if (paging_map(mmio, mmio, PTE_PRESENT | PTE_WRITABLE) < 0) {
        kprintf("ahci: paging_map(%x) failed\n", (unsigned)mmio);
        return;
    }
    g_ahci.abar = (volatile uint8_t *)mmio;

    /* Take control: set AHCI Enable. (Some HBAs come up in legacy IDE
     * mode; on QEMU's ICH9 AHCI it's already AHCI mode at power-on,
     * but the bit is safe to set unconditionally.) */
    hw32(HBA_GHC, hr32(HBA_GHC) | GHC_AE);

    uint32_t cap = hr32(HBA_CAP);
    uint32_t pi  = hr32(HBA_PI);
    kprintf("ahci: HBA @ 0x%x  CAP=0x%x  PI=0x%x  ports=%d slots=%d\n",
            (unsigned)mmio, cap, pi, (cap & 0x1F) + 1,
            ((cap >> 8) & 0x1F) + 1);

    /* Wire IRQ BEFORE port_init so the per-port sanity probe (which
     * runs inside port_init) can use the IRQ-driven completion path.
     * Plays nicely with virtio + e1000 on the same shared PCI line
     * via the isr.c handler chain (session 124). */
    g_ahci.in_use = 1;
    isr_register_irq(g_ahci.pci.irq_line, ahci_irq);
    pic_clear_mask((uint8_t)g_ahci.pci.irq_line);
    /* HBA-level interrupt enable. Without GHC.IE the per-port IS bits
     * still set but the controller won't drive the PCI INTx line. */
    hw32(HBA_GHC, hr32(HBA_GHC) | GHC_IE);

    /* Bring up every implemented port that has a SATA disk. */
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1u << i))) continue;
        port_init(i);
    }

    if (g_ahci.n_attached == 0) {
        kprintf("ahci: no SATA disks attached on any implemented port\n");
    }
}
