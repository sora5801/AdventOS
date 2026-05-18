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

/* Device signatures (P_SIG). */
#define SIG_SATA   0x00000101u

/* SSTS detection (DET, bits 0..3). */
#define DET_PRESENT_NOCOMM  1
#define DET_PRESENT_AND_OK  3

/* ATA commands we use. */
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_IDENTIFY       0xEC

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
    struct ahci_cmd_hdr   *clist;    /* command list (1 KiB) */
    void              *fis;          /* FIS RX area (256 B) */
    struct ahci_cmd_table *ctbl;     /* slot-0 command table */
    spinlock_t         lock;
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

/* Wait until CI bit `slot` clears. Returns 0 on success, -1 on timeout
 * or task-file error. */
static int wait_command(volatile uint8_t *port, int slot) {
    for (int i = 0; i < 5000000; i++) {
        uint32_t ci = pr32(port, P_CI);
        if (!(ci & (1u << slot))) {
            if (pr32(port, P_TFD) & TFD_ERR) return -1;
            return 0;
        }
        if (pr32(port, P_IS) & (1u << 30)) return -1;   /* task-file err */
    }
    return -1;
}

/* Fill a fresh H2D Register FIS into the command table CFIS area. */
static void build_h2d_fis(struct ahci_cmd_table *ct,
                          uint8_t  cmd,
                          uint64_t lba,
                          uint16_t count)
{
    uint8_t *f = ct->cfis;
    memset(f, 0, 64);
    f[0]  = 0x27;                    /* H2D Register FIS */
    f[1]  = 0x80;                    /* C = 1 (command, not control) */
    f[2]  = cmd;
    f[3]  = 0;                       /* features-low */
    f[4]  = (uint8_t)(lba      );
    f[5]  = (uint8_t)(lba >>  8);
    f[6]  = (uint8_t)(lba >> 16);
    f[7]  = 0x40;                    /* LBA mode */
    f[8]  = (uint8_t)(lba >> 24);
    f[9]  = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[11] = 0;                       /* features-high */
    f[12] = (uint8_t)(count      );
    f[13] = (uint8_t)(count >>  8);
}

/* Common DMA path: issue one ATA command with a single PRD pointing at
 * `buf`. `bytes` = data-phase byte count. `write_flag` = 1 for writes. */
static int port_dma_xfer(struct ahci_port *p, uint8_t cmd, uint64_t lba,
                         uint16_t count, void *buf, uint32_t bytes,
                         int write_flag)
{
    spin_lock(&p->lock);

    struct ahci_cmd_hdr *h = &p->clist[0];
    h->flags  = (uint16_t)(5 | (write_flag ? (1u << 6) : 0));  /* CFL=5 + W */
    h->prdtl  = 1;
    h->prdbc  = 0;
    h->ctba   = (uint32_t)(uintptr_t)p->ctbl;
    h->ctbau  = 0;

    build_h2d_fis(p->ctbl, cmd, lba, count);

    /* PRDT[0] -> caller's buffer. */
    p->ctbl->prdt[0].dba  = (uint32_t)(uintptr_t)buf;
    p->ctbl->prdt[0].dbau = 0;
    p->ctbl->prdt[0].rsv  = 0;
    p->ctbl->prdt[0].dbc  = (bytes - 1) & 0x3FFFFFu;   /* I bit clear */

    /* Clear pending IS bits and kick slot 0. */
    pw32(p->regs, P_IS, 0xFFFFFFFFu);
    pw32(p->regs, P_CI, 1u);

    int rc = wait_command(p->regs, 0);
    spin_unlock(&p->lock);
    return rc;
}

/* ---- blkdev adapter -------------------------------------------- */

static int ahci_blkdev_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf) {
    struct ahci_port *p = (struct ahci_port *)d->driver_data;
    if (n == 0) return 0;
    if (n > 256) return -1;          /* count is 16-bit but READ DMA EXT
                                      * caps at 0xFFFF; we keep it small
                                      * because PRD byte count is per-PRD
                                      * and we use one PRD. */
    return port_dma_xfer(p, ATA_CMD_READ_DMA_EXT, lba, (uint16_t)n,
                         buf, n * 512u, 0);
}

static int ahci_blkdev_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf) {
    struct ahci_port *p = (struct ahci_port *)d->driver_data;
    if (n == 0) return 0;
    if (n > 256) return -1;
    return port_dma_xfer(p, ATA_CMD_WRITE_DMA_EXT, lba, (uint16_t)n,
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
    p->ctbl  = (struct ahci_cmd_table *)alloc_aligned(
                  sizeof(struct ahci_cmd_table), 128);
    if (!p->clist || !p->fis || !p->ctbl) {
        kprintf("ahci: port %d alloc failed\n", idx);
        return -1;
    }
    memset(p->clist, 0, 1024);
    memset(p->fis,   0, 256);
    memset(p->ctbl,  0, sizeof(struct ahci_cmd_table));

    pw32(p->regs, P_CLB,  (uint32_t)(uintptr_t)p->clist);
    pw32(p->regs, P_CLBU, 0);
    pw32(p->regs, P_FB,   (uint32_t)(uintptr_t)p->fis);
    pw32(p->regs, P_FBU,  0);

    /* Clear errors and IS. */
    pw32(p->regs, P_SERR, 0xFFFFFFFFu);
    pw32(p->regs, P_IS,   0xFFFFFFFFu);

    port_start(p->regs);

    /* Identify to learn capacity. */
    p->n_sectors = ahci_identify(p);
    if (p->n_sectors == 0) {
        kprintf("ahci: port %d IDENTIFY failed\n", idx);
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
        return -1;
    }
    p->in_use = 1;
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
    kprintf("ahci: HBA @ 0x%x  CAP=0x%x  PI=0x%x  ports=%d\n",
            (unsigned)mmio, cap, pi, (cap & 0x1F) + 1);

    /* Bring up every implemented port that has a SATA disk. */
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1u << i))) continue;
        port_init(i);
    }

    g_ahci.in_use = 1;
    if (g_ahci.n_attached == 0) {
        kprintf("ahci: no SATA disks attached on any implemented port\n");
    }
}
