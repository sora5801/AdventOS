/*
 * Intel 82540EM driver.
 *
 * Register layout: 128 KiB MMIO BAR (BAR0).  We identity-map the BAR
 * pages into the kernel PD and read/write as 32-bit volatile MMIO.
 * Notable registers (offsets from BAR base):
 *
 *   0x00000 CTRL    — device control (reset, link-up, etc.)
 *   0x00008 STATUS  — device status (link state)
 *   0x000C0 ICR     — interrupt cause (read-to-clear)
 *   0x000D0 IMS     — interrupt mask set
 *   0x000D8 IMC     — interrupt mask clear
 *   0x00100 RCTL    — receive control
 *   0x00400 TCTL    — transmit control
 *   0x02800 RDBAL   — RX desc ring base (low 32 bits)
 *   0x02808 RDLEN   — RX desc ring length in bytes
 *   0x02810 RDH     — RX desc ring head (device-advance)
 *   0x02818 RDT     — RX desc ring tail (driver-advance; "last index
 *                                          where descriptor is valid")
 *   0x03800 TDBAL   — TX desc ring base
 *   0x03808 TDLEN   — TX desc ring length
 *   0x03810 TDH     — TX desc ring head
 *   0x03818 TDT     — TX desc ring tail
 *   0x05200 MTA     — multicast filter table (128 dwords; we clear it)
 *   0x05400 RAL0    — receive address low (MAC bytes 0..3)
 *   0x05404 RAH0    — receive address high (MAC bytes 4..5 + AV)
 *
 * Descriptor formats (16 bytes each):
 *
 *   RX:  u64 buf_phys; u16 length; u16 csum; u8 status; u8 errors; u16 vlan;
 *   TX:  u64 buf_phys; u16 length; u8 cso; u8 cmd; u8 sta; u8 css; u16 vlan;
 *
 * Status.DD (descriptor done, bit 0) is the "device finished this
 * entry" signal.  For RX we drain entries with DD set; for TX we
 * advance our head when DD is set (we don't actually wait — TX is
 * fire-and-forget).
 *
 * Rings are 16 entries; buffers are 2048 bytes (max Ethernet frame
 * 1518 + slack).  All allocations come from kmalloc — identity-
 * mapped, phys == virt.
 */
#include "e1000.h"
#include "pci.h"
#include "net.h"
#include "isr.h"
#include "pic.h"
#include "paging.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"
#include "spinlock.h"
#include "../include/io.h"

/* PCI device IDs we recognize. 82540EM is the classic `-device e1000`
 * in QEMU.  82574L is `-device e1000e`. */
#define E1000_VENDOR    0x8086
#define E1000_DEV_82540 0x100E    /* -device e1000   */
#define E1000_DEV_82574 0x10D3    /* -device e1000e  */

/* Register offsets from MMIO base. */
#define R_CTRL    0x00000
#define R_STATUS  0x00008
#define R_ICR     0x000C0
#define R_ITR     0x000C4
#define R_IMS     0x000D0
#define R_IMC     0x000D8
#define R_RCTL    0x00100
#define R_TCTL    0x00400
#define R_TIPG    0x00410
#define R_RDBAL   0x02800
#define R_RDBAH   0x02804
#define R_RDLEN   0x02808
#define R_RDH     0x02810
#define R_RDT     0x02818
#define R_TDBAL   0x03800
#define R_TDBAH   0x03804
#define R_TDLEN   0x03808
#define R_TDH     0x03810
#define R_TDT     0x03818
#define R_MTA     0x05200
#define R_RAL0    0x05400
#define R_RAH0    0x05404

/* CTRL bits */
#define CTRL_SLU       (1u << 6)    /* set link up */
#define CTRL_RST       (1u << 26)   /* reset */

/* RCTL bits */
#define RCTL_EN        (1u << 1)    /* receive enable */
#define RCTL_LBM_NONE  0u
#define RCTL_BAM       (1u << 15)   /* accept broadcast */
#define RCTL_BSIZE_2048 0u          /* bits 16-17 = 00, BSEX = 0 */
#define RCTL_SECRC     (1u << 26)   /* strip Ethernet CRC */

/* TCTL bits */
#define TCTL_EN        (1u << 1)
#define TCTL_PSP       (1u << 3)    /* pad short packets */
#define TCTL_CT_SHIFT  4            /* collision threshold (8 bits) */
#define TCTL_COLD_SHIFT 12          /* collision distance (10 bits) */

/* TX desc cmd bits */
#define TXD_CMD_EOP    (1u << 0)
#define TXD_CMD_IFCS   (1u << 1)    /* insert FCS */
#define TXD_CMD_RS     (1u << 3)    /* report status */

/* Status bits */
#define RXD_STA_DD     (1u << 0)
#define RXD_STA_EOP    (1u << 1)
#define TXD_STA_DD     (1u << 0)

/* ICR bits */
#define ICR_TXDW       (1u << 0)    /* TX desc written */
#define ICR_LSC        (1u << 2)    /* link status change */
#define ICR_RXDMT0     (1u << 4)    /* RX desc min threshold */
#define ICR_RXT0       (1u << 7)    /* RX timer */

#define E1000_RING_LEN 16
#define E1000_BUF_SIZE 2048

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  sta;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

struct e1000 {
    int               in_use;
    struct pci_device pci;
    volatile uint8_t *mmio;            /* mapped BAR0 */
    struct mac_addr   mac;

    struct e1000_rx_desc *rx_ring;     /* 16 entries, 256 bytes */
    struct e1000_tx_desc *tx_ring;     /* 16 entries, 256 bytes */
    uint8_t          *rx_buf[E1000_RING_LEN];
    uint8_t          *tx_buf[E1000_RING_LEN];
    uint16_t          rx_idx;          /* next RX descriptor to consume */
    uint16_t          tx_idx;          /* next TX descriptor to fill */
    spinlock_t        tx_lock;
};

static struct e1000 g_e1k;

/* ---- MMIO helpers ---------------------------------------------- */

static inline uint32_t mr32(uint32_t off) {
    return *(volatile uint32_t *)(g_e1k.mmio + off);
}
static inline void mw32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_e1k.mmio + off) = v;
}

/* ---- IRQ handler ------------------------------------------------ */

static void e1000_irq(struct registers *r) {
    (void)r;
    uint32_t icr = mr32(R_ICR);     /* read-to-clear */
    if (icr == 0) return;

    if (icr & (ICR_RXT0 | ICR_RXDMT0)) {
        /* Drain RX ring. The DD bit on a descriptor's status means
         * the device has DMA'd a packet into our buffer. */
        for (;;) {
            struct e1000_rx_desc *d = &g_e1k.rx_ring[g_e1k.rx_idx];
            if (!(d->status & RXD_STA_DD)) break;
            uint16_t len = d->length;
            if (len >= 14 && len <= 1518) {
                net_rx_frame(g_e1k.rx_buf[g_e1k.rx_idx], len);
            }
            d->status = 0;
            mw32(R_RDT, g_e1k.rx_idx);
            g_e1k.rx_idx = (uint16_t)((g_e1k.rx_idx + 1) % E1000_RING_LEN);
        }
    }
    /* TX completion (ICR_TXDW): nothing to do — we don't wait on
     * sends; the descriptor cycle naturally reuses by the time we
     * wrap.  RS|EOP|DD ack is enough. */
}

/* ---- TX path ---------------------------------------------------- */

int e1000_send(const void *frame, uint32_t len) {
    struct e1000 *e = &g_e1k;
    if (!e->in_use) return -1;
    if (len == 0 || len > 1518) return -1;

    spin_lock(&e->tx_lock);
    uint16_t i = e->tx_idx;
    struct e1000_tx_desc *d = &e->tx_ring[i];

    /* If the slot we're about to use is still owned by the device
     * (DD bit clear AND we've already written it once), wait briefly.
     * In practice for 16-slot rings on SLIRP this never blocks. */
    if (d->cmd != 0 && !(d->sta & TXD_STA_DD)) {
        for (int spin = 0; spin < 1000000; spin++) {
            if (d->sta & TXD_STA_DD) break;
        }
    }

    memcpy(e->tx_buf[i], frame, len);
    d->addr   = (uint64_t)(uintptr_t)e->tx_buf[i];
    d->length = (uint16_t)len;
    d->cso    = 0;
    d->cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->sta    = 0;
    d->css    = 0;
    d->special = 0;

    e->tx_idx = (uint16_t)((i + 1) % E1000_RING_LEN);
    mw32(R_TDT, e->tx_idx);
    spin_unlock(&e->tx_lock);
    return (int)len;
}

/* ---- init ------------------------------------------------------- */

int e1000_init(struct mac_addr *out_mac) {
    struct e1000 *e = &g_e1k;
    if (e->in_use) return -1;

    /* Probe both the 82540EM and 82574L PCI ids. */
    if (pci_find(E1000_VENDOR, E1000_DEV_82540, &e->pci) != 0 &&
        pci_find(E1000_VENDOR, E1000_DEV_82574, &e->pci) != 0)
    {
        return -1;
    }

    /* BAR0 is MMIO — low bit clear.  Mask off the type bits and
     * identity-map a 128 KiB window so we can talk to registers. */
    if (e->pci.bar0 & 1u) {
        kprintf("e1000: BAR0 is I/O-space (0x%x), expected MMIO\n",
                (unsigned)e->pci.bar0);
        return -1;
    }
    uintptr_t mmio_phys = (uintptr_t)(e->pci.bar0 & ~0xFu);
    for (uintptr_t p = mmio_phys; p < mmio_phys + 0x20000; p += 0x1000) {
        if (paging_map(p, p, PTE_PRESENT | PTE_WRITABLE) < 0) {
            kprintf("e1000: paging_map(%x) failed\n", (unsigned)p);
            return -1;
        }
    }
    e->mmio = (volatile uint8_t *)mmio_phys;
    spin_lock_init(&e->tx_lock);

    kprintf("e1000: PCI %u:%u.%u  dev=0x%x  mmio=0x%x  irq=%u\n",
            e->pci.bus, e->pci.device, e->pci.func,
            (unsigned)e->pci.device_id, (unsigned)mmio_phys,
            (unsigned)e->pci.irq_line);

    /* Reset.  CTRL.RST self-clears once the device is back. */
    mw32(R_CTRL, mr32(R_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000; i++) {
        if (!(mr32(R_CTRL) & CTRL_RST)) break;
    }

    /* Mask all interrupts during setup. */
    mw32(R_IMC, 0xFFFFFFFFu);
    (void)mr32(R_ICR);          /* clear pending */

    /* Read MAC from RAL0/RAH0.  QEMU pre-loads these from the
     * `mac=` device option; on real hardware they'd come out of
     * the EEPROM.  Either way the device latches them into the
     * receive-address registers at power-on, so we just read. */
    uint32_t ral = mr32(R_RAL0);
    uint32_t rah = mr32(R_RAH0);
    e->mac.b[0] = (uint8_t)(ral      );
    e->mac.b[1] = (uint8_t)(ral >>  8);
    e->mac.b[2] = (uint8_t)(ral >> 16);
    e->mac.b[3] = (uint8_t)(ral >> 24);
    e->mac.b[4] = (uint8_t)(rah      );
    e->mac.b[5] = (uint8_t)(rah >>  8);

    /* Clear multicast filter table (128 dwords). */
    for (int i = 0; i < 128; i++) mw32(R_MTA + i * 4, 0);

    /* RX ring.  Descriptors must be 16-byte aligned and contiguous. */
    e->rx_ring = (struct e1000_rx_desc *)
        kmalloc(sizeof(struct e1000_rx_desc) * E1000_RING_LEN);
    if (!e->rx_ring) return -1;
    for (int i = 0; i < E1000_RING_LEN; i++) {
        e->rx_buf[i] = kmalloc(E1000_BUF_SIZE);
        if (!e->rx_buf[i]) return -1;
        e->rx_ring[i].addr   = (uint64_t)(uintptr_t)e->rx_buf[i];
        e->rx_ring[i].length = 0;
        e->rx_ring[i].status = 0;
        e->rx_ring[i].errors = 0;
    }
    mw32(R_RDBAL, (uint32_t)(uintptr_t)e->rx_ring);
    mw32(R_RDBAH, 0);
    mw32(R_RDLEN, sizeof(struct e1000_rx_desc) * E1000_RING_LEN);
    mw32(R_RDH, 0);
    mw32(R_RDT, E1000_RING_LEN - 1);   /* tail = last valid index */
    e->rx_idx = 0;
    mw32(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);

    /* TX ring. */
    e->tx_ring = (struct e1000_tx_desc *)
        kmalloc(sizeof(struct e1000_tx_desc) * E1000_RING_LEN);
    if (!e->tx_ring) return -1;
    for (int i = 0; i < E1000_RING_LEN; i++) {
        e->tx_buf[i] = kmalloc(E1000_BUF_SIZE);
        if (!e->tx_buf[i]) return -1;
        e->tx_ring[i].addr   = 0;
        e->tx_ring[i].cmd    = 0;
        e->tx_ring[i].sta    = 0;
    }
    mw32(R_TDBAL, (uint32_t)(uintptr_t)e->tx_ring);
    mw32(R_TDBAH, 0);
    mw32(R_TDLEN, sizeof(struct e1000_tx_desc) * E1000_RING_LEN);
    mw32(R_TDH, 0);
    mw32(R_TDT, 0);
    e->tx_idx = 0;
    /* Collision threshold 0x10, distance 0x40.  Defaults from
     * 82540EM datasheet §13.4.34. */
    mw32(R_TCTL, TCTL_EN | TCTL_PSP |
                 (0x10u << TCTL_CT_SHIFT) |
                 (0x40u << TCTL_COLD_SHIFT));
    /* Inter-packet gap: 10 bits each for IPGT / IPGR1 / IPGR2 with
     * recommended values 10 / 8 / 6 for 1000BASE-T half-duplex. */
    mw32(R_TIPG, 10 | (8u << 10) | (6u << 20));

    /* Link up. */
    mw32(R_CTRL, mr32(R_CTRL) | CTRL_SLU);

    /* Hook IRQ + unmask the events we care about. */
    isr_register_irq(e->pci.irq_line, e1000_irq);
    pic_clear_mask(e->pci.irq_line);
    mw32(R_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_TXDW | ICR_LSC);

    e->in_use = 1;
    if (out_mac) *out_mac = e->mac;

    kprintf("e1000: link up — MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            e->mac.b[0], e->mac.b[1], e->mac.b[2],
            e->mac.b[3], e->mac.b[4], e->mac.b[5]);
    return 0;
}
