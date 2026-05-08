#include "rtl8139.h"
#include "pci.h"
#include "isr.h"
#include "pic.h"
#include "kmalloc.h"
#include "string.h"
#include "kprintf.h"
#include "spinlock.h"
#include "../include/io.h"

/* RTL8139 register offsets from the I/O base. */
#define R_MAC0      0x00      /* 6 bytes — board MAC */
#define R_RBSTART   0x30      /* RX buffer physical base, 4 bytes */
#define R_CR        0x37      /* Command */
#define R_CAPR      0x38      /* Current Address of Packet Read */
#define R_IMR       0x3C      /* Interrupt Mask */
#define R_ISR       0x3E      /* Interrupt Status */
#define R_TCR       0x40      /* Transmit Configuration */
#define R_RCR       0x44      /* Receive Configuration */
#define R_CONFIG1   0x52

/* CR bits */
#define CR_BUFE     0x01
#define CR_TE       0x04
#define CR_RE       0x08
#define CR_RST      0x10

/* TX descriptor registers (4 of each) */
#define R_TSD(i)    (0x10 + (i) * 4)   /* Transmit Status of Descriptor i */
#define R_TSAD(i)   (0x20 + (i) * 4)   /* Transmit Start Address of Desc i */

/* TSD bits */
#define TSD_OWN     (1u << 13)         /* Set by NIC when DMA done */
#define TSD_TOK     (1u << 15)         /* Transmit OK */
#define TSD_ERR     ((1u << 30) | (1u << 29) | (1u << 28))

/* ISR bits */
#define ISR_ROK     0x0001
#define ISR_RER     0x0002
#define ISR_TOK     0x0004
#define ISR_TER     0x0008

/* RCR bits */
#define RCR_AAP     0x01     /* accept all packets (promiscuous) */
#define RCR_APM     0x02     /* accept physical match */
#define RCR_AM      0x04     /* accept multicast */
#define RCR_AB      0x08     /* accept broadcast */
#define RCR_WRAP    0x80     /* wrap mode (no boundary handling needed) */

#define RX_BUF_LEN  (8192 + 16 + 1500)
#define TX_BUF_LEN  2048
#define NUM_TX      4

static struct pci_device g_pci;
static uint16_t          g_io;
static uint8_t          *g_rx_buf;
static uint8_t          *g_tx_buf[NUM_TX];
static uint32_t          g_rx_ptr;       /* offset into RX ring we've consumed */
static int               g_tx_cur;       /* round-robin TX descriptor index */
static spinlock_t        g_tx_lock = SPINLOCK_INIT;

static void rtl_irq(struct registers *r) {
    (void)r;
    uint16_t isr = inw(g_io + R_ISR);
    /* Acknowledge — write the bits back to clear them. */
    outw(g_io + R_ISR, isr);

    if (isr & ISR_ROK) {
        /* Drain RX ring. */
        while (!(inb(g_io + R_CR) & CR_BUFE)) {
            uint8_t *pkt = g_rx_buf + g_rx_ptr;
            uint16_t status = (uint16_t)pkt[0] | ((uint16_t)pkt[1] << 8);
            uint16_t length = (uint16_t)pkt[2] | ((uint16_t)pkt[3] << 8);

            if ((status & 1) && length >= 14 && length <= 1518) {
                /* Hand the frame (after the 4-byte RTL header, minus
                 * trailing 4-byte FCS) up to the network layer. */
                net_rx_frame(pkt + 4, (uint32_t)(length - 4));
            }

            /* Round up to 4 bytes and advance the read pointer. */
            g_rx_ptr = (g_rx_ptr + length + 4 + 3) & ~3u;
            if (g_rx_ptr >= RX_BUF_LEN) g_rx_ptr -= RX_BUF_LEN;

            outw(g_io + R_CAPR, (uint16_t)(g_rx_ptr - 16));
        }
    }
}

int rtl8139_init(struct mac_addr *out_mac) {
    if (pci_find(0x10EC, 0x8139, &g_pci) != 0) return -1;
    g_io = g_pci.io_base;

    /* Power on, then reset and wait for completion. */
    outb(g_io + R_CONFIG1, 0x00);
    outb(g_io + R_CR, CR_RST);
    for (int i = 0; i < 1000000 && (inb(g_io + R_CR) & CR_RST); i++) {}

    /* Read MAC. */
    for (int i = 0; i < 6; i++) out_mac->b[i] = inb(g_io + R_MAC0 + i);

    /* Allocate DMA buffers. kmalloc returns kernel heap addresses
     * which are in our identity-mapped region (phys == virt), so we
     * can hand them straight to the NIC. */
    g_rx_buf = kzalloc(RX_BUF_LEN);
    if (!g_rx_buf) return -1;
    for (int i = 0; i < NUM_TX; i++) {
        g_tx_buf[i] = kzalloc(TX_BUF_LEN);
        if (!g_tx_buf[i]) return -1;
        outl(g_io + R_TSAD(i), (uint32_t)(uintptr_t)g_tx_buf[i]);
    }

    outl(g_io + R_RBSTART, (uint32_t)(uintptr_t)g_rx_buf);

    /* Hook IRQ before unmasking it on the PIC. */
    isr_register_irq(g_pci.irq_line, rtl_irq);
    pic_clear_mask(g_pci.irq_line);

    /* Enable RX OK + TX OK interrupts. */
    outw(g_io + R_IMR, ISR_ROK | ISR_TOK);

    /* RX config: accept broadcast/multicast/physical-match,
     * promiscuous so SLIRP's gateway responses always land. WRAP=1
     * so the NIC writes past end-of-buffer rather than wrapping —
     * we sized the buffer with 1500 bytes of slack for this. */
    outl(g_io + R_RCR,
         RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);

    /* TX config defaults are fine for QEMU; just kick CR. */
    outb(g_io + R_CR, CR_TE | CR_RE);

    g_rx_ptr = 0;
    g_tx_cur = 0;

    kprintf("rtl8139: PCI %u:%u.%u  io=0x%x  irq=%u\n",
            g_pci.bus, g_pci.device, g_pci.func,
            (unsigned)g_io, g_pci.irq_line);
    return 0;
}

int rtl8139_send(const void *frame, uint32_t len) {
    if (len < 60)   len = 60;       /* pad short frames to min ETH length */
    if (len > TX_BUF_LEN) return -1;

    spin_lock(&g_tx_lock);
    int slot = g_tx_cur;
    g_tx_cur = (g_tx_cur + 1) % NUM_TX;

    /* Wait for any prior TX in this slot to complete (OWN=1). */
    for (int i = 0; i < 1000000; i++) {
        if (inl(g_io + R_TSD(slot)) & TSD_OWN) break;
    }

    memcpy(g_tx_buf[slot], frame, len);
    /* Some sources say to also poke TSAD here; we set it once at init.
     * Writing length+OWN-clear into TSD kicks the actual transfer. */
    outl(g_io + R_TSD(slot), len & 0x1FFF);

    spin_unlock(&g_tx_lock);
    return (int)len;
}
