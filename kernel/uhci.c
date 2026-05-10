/*
 * UHCI 1.1 host controller driver.
 *
 * Quick UHCI tour, since this driver is tiny only because the
 * spec is well-defined:
 *
 *   - Frame list: 1024 × 32-bit physical pointers, 4 KiB total,
 *     4 KiB-aligned. Each entry points to a Queue Head (QH) or
 *     Transfer Descriptor (TD). The hardware advances FRNUM (frame
 *     number) once per millisecond and walks frame_list[FRNUM].
 *
 *   - Queue Head (QH): 8 bytes hardware, 16-byte aligned. Two link
 *     pointers — head_link to the next QH in the schedule, and
 *     element_link to the first TD in this queue's pending chain.
 *
 *   - Transfer Descriptor (TD): 16 bytes hardware, 16-byte aligned.
 *     Has a link to the next TD, a status word (active/error/length
 *     written by HW), a token (PID/addr/ep/maxlen/toggle), and a
 *     buffer pointer (physical).
 *
 *   - Link encoding: bit 0 = Terminate, bit 1 = QH (vs TD), bit 2
 *     = Vertical-First (depth — used for chaining TDs). Bits 4..31
 *     are the 16-byte-aligned physical address.
 *
 * Our schedule: every frame_list[i] points to ONE schedule QH. That
 * QH's element_link is the queue we add transfers to. After a
 * transfer chain completes, we reset element_link to TERMINATE.
 * Simple, single-threaded — no isochronous, no interrupt schedule
 * (we poll), no multi-controller. Enough for keyboard + (next
 * session) mass storage.
 *
 * Identity-mapped kernel makes physical-vs-virtual a non-issue.
 */
#include "uhci.h"
#include "pci.h"
#include "kmalloc.h"
#include "pmm.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"
#include "../include/io.h"

/* ---- PCI IDs we look for --------------------------------------- */

#define UHCI_VENDOR_INTEL   0x8086
#define UHCI_DEVICE_PIIX3   0x7020      /* QEMU's default `-usb` */
#define UHCI_DEVICE_PIIX4   0x7112

/* ---- Register offsets from io_base ----------------------------- */

#define UHCI_USBCMD     0x00      /* 16-bit */
#define UHCI_USBSTS     0x02      /* 16-bit */
#define UHCI_USBINTR    0x04      /* 16-bit */
#define UHCI_FRNUM      0x06      /* 16-bit */
#define UHCI_FRBASEADD  0x08      /* 32-bit, must be 4 KiB aligned */
#define UHCI_SOFMOD     0x0C      /*  8-bit */
#define UHCI_PORTSC1    0x10      /* 16-bit */
#define UHCI_PORTSC2    0x12      /* 16-bit */

/* USBCMD bits */
#define UHCI_CMD_RUN        (1 << 0)
#define UHCI_CMD_HCRESET    (1 << 1)
#define UHCI_CMD_GRESET     (1 << 2)
#define UHCI_CMD_MAXP       (1 << 7)    /* 64-byte max packet */

/* USBSTS bits */
#define UHCI_STS_USBINT     (1 << 0)
#define UHCI_STS_ERROR      (1 << 1)
#define UHCI_STS_HCHALTED   (1 << 5)

/* PORTSC bits */
#define UHCI_PORT_CONNECT   (1 << 0)
#define UHCI_PORT_CSC       (1 << 1)    /* connect status change */
#define UHCI_PORT_ENABLE    (1 << 2)
#define UHCI_PORT_PEC       (1 << 3)    /* port enable change */
#define UHCI_PORT_LOWSPEED  (1 << 8)
#define UHCI_PORT_RESET     (1 << 9)
/* Mask of write-1-to-clear status bits — must clear when writing
 * other bits, otherwise an unrelated update accidentally clears
 * outstanding change indications. */
#define UHCI_PORT_WC_MASK   (UHCI_PORT_CSC | UHCI_PORT_PEC)

/* ---- Link pointer bits ----------------------------------------- */

#define UHCI_LINK_TERMINATE (1 << 0)
#define UHCI_LINK_QH        (1 << 1)
#define UHCI_LINK_DEPTH     (1 << 2)    /* TD-only: process this branch first */

/* ---- TD status word -------------------------------------------- */

#define UHCI_TD_STS_ACTLEN(s)   ((s) & 0x7FF)
#define UHCI_TD_STS_BITSTUFF    (1 << 17)
#define UHCI_TD_STS_CRC         (1 << 18)
#define UHCI_TD_STS_NAK         (1 << 19)
#define UHCI_TD_STS_BABBLE      (1 << 20)
#define UHCI_TD_STS_DBE         (1 << 21)
#define UHCI_TD_STS_STALLED     (1 << 22)
#define UHCI_TD_STS_ACTIVE      (1 << 23)
#define UHCI_TD_STS_IOC         (1 << 24)
#define UHCI_TD_STS_IOS         (1 << 25)
#define UHCI_TD_STS_LOWSPEED    (1 << 26)
#define UHCI_TD_STS_ERR_MASK    (UHCI_TD_STS_BITSTUFF | UHCI_TD_STS_CRC | \
                                 UHCI_TD_STS_BABBLE | UHCI_TD_STS_DBE  | \
                                 UHCI_TD_STS_STALLED)

/* Encode error count (3 retries) into bits 27..28. */
#define UHCI_TD_STS_CERR_3      (3 << 27)

/* TD token PIDs */
#define UHCI_PID_SETUP      0x2D
#define UHCI_PID_IN         0x69
#define UHCI_PID_OUT        0xE1

/* MaxLen field encoding: actual length minus 1 (0..0x7FE), or
 * 0x7FF for "zero bytes". Stored in bits 21..31 of the token. */
#define UHCI_TD_TOK_MAXLEN_ZERO  0x7FF

/* ---- Hardware structs ------------------------------------------ */

struct uhci_td {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
    /* Software-only pad to 32 bytes (the next TD lands on a fresh
     * 16-byte boundary either way; 32 just keeps things tidy). */
    uint32_t pad[4];
} __attribute__((aligned(16)));

struct uhci_qh {
    volatile uint32_t head_link;
    volatile uint32_t element_link;
    /* Pad to 16 bytes — head_link and element_link are the only
     * two HW-visible fields. */
    uint32_t pad[2];
} __attribute__((aligned(16)));

/* ---- Driver state ---------------------------------------------- */

#define UHCI_TD_POOL_SIZE   16    /* max TDs per outstanding transfer */

static struct pci_device   g_pci;
static int                 g_present;
static uint16_t            g_io;
static uint32_t           *g_frame_list;     /* 1024 * 4B */
static struct uhci_qh     *g_qh;             /* the one schedule QH */
/* The TD pool is heap-allocated so we can guarantee 16-byte
 * alignment (UHCI requires it; static-BSS aligned attributes
 * don't always survive the kernel link). */
static struct uhci_td     *g_td_pool;

/* ---- Tiny helpers ---------------------------------------------- */

static inline uint32_t phys(const void *p) { return (uint32_t)p; }

static void udelay(int us) {
    /* We have no fine-grained timer. io_wait() is ~1µs on real
     * hardware, considerably less on QEMU. For QEMU correctness
     * we err on the side of more iterations. */
    for (int i = 0; i < us; i++) io_wait();
}

/* Wait until the last TD in `chain` has cleared ACTIVE, or timeout.
 * Returns 0 on success, USB_ERR_TIMEOUT / STALL / OTHER. */
static int wait_chain(struct uhci_td *last, int timeout_ms) {
    /* Spin briefly each ms — UHCI advances frames once per ms, so
     * a transfer that's going to complete this frame will do so in
     * <1 ms of spinning. */
    for (int ms = 0; ms < timeout_ms; ms++) {
        for (int i = 0; i < 1000; i++) {
            if (!(last->status & UHCI_TD_STS_ACTIVE)) goto done;
            udelay(1);
        }
    }
    return USB_ERR_TIMEOUT;
done:
    if (last->status & UHCI_TD_STS_STALLED) return USB_ERR_STALL;
    if (last->status & UHCI_TD_STS_BABBLE)  return USB_ERR_BABBLE;
    if (last->status & UHCI_TD_STS_CRC)     return USB_ERR_CRC;
    if (last->status & UHCI_TD_STS_ERR_MASK) return USB_ERR_OTHER;
    return USB_OK;
}

/* Build a TD's token field (bits 21..31 = max-len, bit 19 = toggle,
 * bits 15..18 = endpoint, bits 8..14 = address, bits 0..7 = PID). */
static uint32_t make_token(uint8_t pid, uint8_t addr, uint8_t ep,
                           int toggle, int len) {
    uint32_t maxlen = (len == 0)
        ? UHCI_TD_TOK_MAXLEN_ZERO
        : (uint32_t)((len - 1) & 0x7FF);
    return (maxlen << 21)
         | ((toggle & 1) << 19)
         | ((ep & 0xF) << 15)
         | ((addr & 0x7F) << 8)
         | pid;
}

static uint32_t make_status(int low_speed, int ioc) {
    uint32_t s = UHCI_TD_STS_ACTIVE | UHCI_TD_STS_CERR_3;
    if (low_speed) s |= UHCI_TD_STS_LOWSPEED;
    if (ioc)       s |= UHCI_TD_STS_IOC;
    return s;
}

/* ---- Init ------------------------------------------------------- */

int uhci_init(void) {
    /* Probe both PIIX3 and PIIX4 IDs since QEMU has used both. */
    if (pci_find(UHCI_VENDOR_INTEL, UHCI_DEVICE_PIIX3, &g_pci) != 0 &&
        pci_find(UHCI_VENDOR_INTEL, UHCI_DEVICE_PIIX4, &g_pci) != 0) {
        return -1;
    }

    /* PIIX3/4 UHCI puts its I/O base at PCI BAR4 (offset 0x20),
     * NOT BAR0. The pci_find helper only fishes out BAR0/BAR1
     * for the rtl8139 / AC97 cases. Read BAR4 ourselves. */
    uint32_t bar4 = pci_config_read32(g_pci.bus, g_pci.device, g_pci.func, 0x20);
    if ((bar4 & 1) == 0) return -1;     /* must be I/O space */
    g_io = (uint16_t)(bar4 & ~0x3u);
    if (g_io == 0) return -1;

    /* Take ownership from BIOS — clear LEGSUP register at PCI
     * config offset 0xC0 (USB Legacy Support, PIIX-specific).
     * Writing 0x8F00 clears all status bits, leaving emulation off. */
    pci_config_write32(g_pci.bus, g_pci.device, g_pci.func, 0xC0, 0x8F00);

    /* Global reset: hold GRESET ~50 ms, then clear and HCRESET. */
    outw(g_io + UHCI_USBCMD, UHCI_CMD_GRESET);
    pit_sleep(60);
    outw(g_io + UHCI_USBCMD, 0);

    outw(g_io + UHCI_USBCMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        if (!(inw(g_io + UHCI_USBCMD) & UHCI_CMD_HCRESET)) break;
        pit_sleep(1);
    }

    /* Disable interrupts — we poll. */
    outw(g_io + UHCI_USBINTR, 0);
    /* Clear status bits (write-1-to-clear). */
    outw(g_io + UHCI_USBSTS, 0xFFFF);
    /* SOF timing default. */
    outb(g_io + UHCI_SOFMOD, 0x40);

    /* Frame list: 4 KiB, 4 KiB-aligned. Over-allocate from kmalloc
     * (heap, identity-mapped, doesn't compete for low-memory
     * pages) and round up. QH and TD pool only need 16-byte
     * alignment, which kmalloc gives by default. */
    void *fl_raw = kmalloc(8192);
    if (!fl_raw) return -1;
    g_frame_list = (uint32_t *)(((uint32_t)fl_raw + 0xFFF) & ~0xFFFu);

    g_qh = (struct uhci_qh *)kmalloc(sizeof(*g_qh));
    if (!g_qh) return -1;
    if (((uint32_t)g_qh & 0xF) != 0) return -1;

    g_td_pool = (struct uhci_td *)kmalloc(sizeof(struct uhci_td) * UHCI_TD_POOL_SIZE);
    if (!g_td_pool) return -1;
    if (((uint32_t)g_td_pool & 0xF) != 0) return -1;

    /* Schedule QH starts empty: head terminates, element terminates. */
    g_qh->head_link    = UHCI_LINK_TERMINATE;
    g_qh->element_link = UHCI_LINK_TERMINATE;
    g_qh->pad[0] = g_qh->pad[1] = 0;

    /* Every frame slot points to the schedule QH. */
    uint32_t qh_link = phys(g_qh) | UHCI_LINK_QH;
    for (int i = 0; i < 1024; i++) g_frame_list[i] = qh_link;

    outl(g_io + UHCI_FRBASEADD, phys(g_frame_list));
    outw(g_io + UHCI_FRNUM, 0);

    /* Set RUN bit (and MAXP=64 for full-speed; ignored for low-speed). */
    outw(g_io + UHCI_USBCMD, UHCI_CMD_RUN | UHCI_CMD_MAXP);

    /* Wait until controller reports running (HCHALTED clears). */
    for (int i = 0; i < 100; i++) {
        if (!(inw(g_io + UHCI_USBSTS) & UHCI_STS_HCHALTED)) break;
        pit_sleep(1);
    }

    g_present = 1;
    kprintf("[uhci] PIIX3 controller @ I/O 0x%x  IRQ %d\n",
            g_io, g_pci.irq_line);
    return 0;
}

int uhci_present(void) { return g_present; }

/* ---- Port handling --------------------------------------------- */

static void port_reset(uint16_t port_reg) {
    /* Pulse RESET high for 50ms (USB spec calls for ≥10ms; QEMU and
     * real silicon both accept 50). Clear the WtC change bits so we
     * don't accidentally lose them during the read-modify-write. */
    uint16_t v = inw(g_io + port_reg);
    v &= ~UHCI_PORT_WC_MASK;
    v |= UHCI_PORT_RESET;
    outw(g_io + port_reg, v);
    pit_sleep(50);

    v = inw(g_io + port_reg);
    v &= ~(UHCI_PORT_WC_MASK | UHCI_PORT_RESET);
    outw(g_io + port_reg, v);

    /* Recovery: wait, then enable. The port can take a few ms to
     * stabilize; some devices need explicit Enable, some come up
     * already enabled by the controller. Either way, set Enable
     * and wait for it to stick. */
    pit_sleep(10);
    for (int i = 0; i < 16; i++) {
        v = inw(g_io + port_reg);
        v &= ~UHCI_PORT_WC_MASK;
        v |= UHCI_PORT_ENABLE;
        outw(g_io + port_reg, v);
        pit_sleep(2);
        if (inw(g_io + port_reg) & UHCI_PORT_ENABLE) break;
    }

    /* Acknowledge any change bits set during reset. */
    v = inw(g_io + port_reg);
    outw(g_io + port_reg, v | UHCI_PORT_WC_MASK);
}

void uhci_probe_ports(int *connected, int *low_speed, int *n_ports) {
    *n_ports = 2;
    if (!g_present) {
        connected[0] = connected[1] = 0;
        return;
    }

    uint16_t pregs[2] = { UHCI_PORTSC1, UHCI_PORTSC2 };
    for (int i = 0; i < 2; i++) {
        uint16_t v = inw(g_io + pregs[i]);
        if (v & UHCI_PORT_CONNECT) {
            port_reset(pregs[i]);
            v = inw(g_io + pregs[i]);
            connected[i] = (v & UHCI_PORT_CONNECT) ? 1 : 0;
            low_speed[i] = (v & UHCI_PORT_LOWSPEED) ? 1 : 0;
        } else {
            connected[i] = 0;
            low_speed[i] = 0;
        }
    }
}

/* ---- Control transfer ------------------------------------------ */

int uhci_control_transfer(uint8_t addr, int low_speed, int ep0_max,
                          const struct usb_setup_packet *setup,
                          void *data, int data_len, int data_in)
{
    if (!g_present) return USB_ERR_OTHER;
    if (ep0_max <= 0) ep0_max = 8;

    /* Worst-case TD count: 1 SETUP + ceil(data_len / ep0_max) + 1 STATUS.
     * Cap on UHCI_TD_POOL_SIZE. */
    int n_data_tds = (data_len + ep0_max - 1) / ep0_max;
    int n_total    = 1 + n_data_tds + 1;
    if (n_total > UHCI_TD_POOL_SIZE) return USB_ERR_OTHER;

    /* Walk the pool, building the chain. */
    int idx = 0;
    struct uhci_td *first = &g_td_pool[idx];

    /* SETUP TD — 8 bytes, DATA0 toggle, address whatever caller said. */
    {
        struct uhci_td *t = &g_td_pool[idx++];
        t->status = make_status(low_speed, 0);
        t->token  = make_token(UHCI_PID_SETUP, addr, 0, 0, 8);
        t->buffer = phys(setup);
        /* link patched below */
    }

    /* DATA TDs — DATA1 first, alternating. */
    int toggle = 1;
    int remaining = data_len;
    uint8_t *p = (uint8_t *)data;
    uint8_t pid_data = data_in ? UHCI_PID_IN : UHCI_PID_OUT;
    for (int i = 0; i < n_data_tds; i++) {
        int chunk = remaining > ep0_max ? ep0_max : remaining;
        struct uhci_td *t = &g_td_pool[idx++];
        t->status = make_status(low_speed, 0);
        t->token  = make_token(pid_data, addr, 0, toggle, chunk);
        t->buffer = phys(p);
        p         += chunk;
        remaining -= chunk;
        toggle    ^= 1;
    }

    /* STATUS TD — opposite direction, 0 bytes, DATA1, IOC. */
    struct uhci_td *last;
    {
        last = &g_td_pool[idx++];
        last->status = make_status(low_speed, 1);
        last->token  = make_token(data_in ? UHCI_PID_OUT : UHCI_PID_IN,
                                  addr, 0, 1, 0);
        last->buffer = 0;
    }

    /* Chain links — depth-first so HW processes them in order. */
    for (int i = 0; i < idx - 1; i++) {
        g_td_pool[i].link = phys(&g_td_pool[i + 1]) | UHCI_LINK_DEPTH;
    }
    g_td_pool[idx - 1].link = UHCI_LINK_TERMINATE;

    /* Hand off to the controller by pointing the QH at the first TD.
     * Once the QH's element_link is non-terminate, the controller will
     * process the chain at the next frame boundary. */
    g_qh->element_link = phys(first);

    int rc = wait_chain(last, 1000);

    /* Detach. Even on failure we want the queue back to empty so the
     * next transfer doesn't pick up our half-finished TDs. */
    g_qh->element_link = UHCI_LINK_TERMINATE;
    return rc;
}

/* ---- Single interrupt-IN transfer ------------------------------ */

int uhci_int_in(uint8_t addr, int low_speed, int ep_max,
                int ep, void *buf, int max_len, int *toggle)
{
    if (!g_present) return USB_ERR_OTHER;
    if (max_len > ep_max) max_len = ep_max;

    struct uhci_td *t = &g_td_pool[0];
    t->status = make_status(low_speed, 1);
    t->token  = make_token(UHCI_PID_IN, addr, ep, *toggle, max_len);
    t->buffer = phys(buf);
    t->link   = UHCI_LINK_TERMINATE;

    g_qh->element_link = phys(t);

    /* Short timeout — interrupt endpoints NAK quickly when there's
     * no new data, and we don't want to block the polling task. */
    int rc = wait_chain(t, 50);
    g_qh->element_link = UHCI_LINK_TERMINATE;

    if (rc == USB_ERR_TIMEOUT) return USB_ERR_NAK;
    if (rc != USB_OK) return rc;

    /* On success the data toggle should advance. ActLen is in bits
     * 0..10 of status, encoded as actual_bytes - 1; sentinel 0x7FF
     * means "zero bytes were transferred" (NAK that managed to flip
     * status anyway — treat as NAK). */
    uint32_t actlen_field = UHCI_TD_STS_ACTLEN(t->status);
    if (actlen_field == 0x7FF) return USB_ERR_NAK;
    *toggle ^= 1;
    return (int)(actlen_field + 1);
}
