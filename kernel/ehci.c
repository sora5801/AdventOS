/*
 * EHCI driver. See ehci.h.
 *
 * Register cheat-sheet:
 *
 *   Capability registers (BAR0):
 *     0x00 CAPLENGTH   1B  operational regs start at BAR0 + CAPLENGTH
 *     0x02 HCIVERSION  2B  e.g. 0x0100
 *     0x04 HCSPARAMS   4B  [3:0] N_PORTS, [11:8] N_PCC, etc.
 *     0x08 HCCPARAMS   4B  [7:0] reserved, [15:8] EECP (extended caps ptr)
 *
 *   Operational registers (BAR0 + CAPLENGTH):
 *     0x00 USBCMD      4B  [0] RS, [1] HCReset, [5] PSE, [6] ASE,
 *                          [7] IAAD, [23:16] Interrupt Threshold
 *     0x04 USBSTS      4B  [0] USBINT, [12] HCHalted, [13] Reclamation,
 *                          [14] PSS, [15] ASS
 *     0x08 USBINTR     4B
 *     0x14 PERIODICLISTBASE
 *     0x18 ASYNCLISTADDR
 *     0x40 CONFIGFLAG  4B  [0] = 1 -> EHCI owns ports (else companion)
 *     0x44+ PORTSC[n]  4B  per-port status/control
 *
 * Bring-up:
 *   1. PCI probe (try QEMU's Intel 0x8086:0x24CD first, fall back to
 *      class-code 0x0C/0x03/0x20 scan via vendor+device pairs we know).
 *   2. Map BAR0 MMIO (4 KiB is plenty — caps are ~16 B, op regs +
 *      ports fit in <256 B).
 *   3. EECP walk: find USBLEGSUP cap, write OS-owned semaphore, poll
 *      until BIOS-owned semaphore clears.
 *   4. USBCMD = 0; wait HCHalted = 1.
 *   5. USBCMD.HCReset = 1; poll until 0.
 *   6. Allocate the async placeholder QH (4 KiB aligned, 1 page).
 *   7. Write ASYNCLISTADDR.
 *   8. USBCMD = RS | ASE.
 *   9. CONFIGFLAG = 1 (route ports to EHCI rather than companion).
 *  10. Survey each PORTSC for connected devices.
 */
#include "ehci.h"
#include "pci.h"
#include "kmalloc.h"
#include "paging.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"

/* QEMU's EHCI implementations expose one of these device IDs. */
struct ehci_devid { uint16_t vendor; uint16_t device; const char *name; };
static const struct ehci_devid g_known[] = {
    { 0x8086, 0x24CD, "Intel 82801DB ICH4 EHCI" },     /* `-device usb-ehci` */
    { 0x8086, 0x293A, "Intel 82801IH ICH9 EHCI #1" },  /* `-device ich9-usb-ehci1` */
    { 0x8086, 0x293C, "Intel 82801IH ICH9 EHCI #2" },
    { 0x8086, 0x265C, "Intel 82801FB ICH6 EHCI" },
    { 0, 0, 0 }
};

/* Capability offsets. */
#define CAP_CAPLENGTH    0x00
#define CAP_HCIVERSION   0x02
#define CAP_HCSPARAMS    0x04
#define CAP_HCCPARAMS    0x08

/* Operational offsets (added to op_base). */
#define OP_USBCMD            0x00
#define OP_USBSTS            0x04
#define OP_USBINTR           0x08
#define OP_FRINDEX           0x0C
#define OP_CTRLDSSEGMENT     0x10
#define OP_PERIODICLISTBASE  0x14
#define OP_ASYNCLISTADDR     0x18
#define OP_CONFIGFLAG        0x40
#define OP_PORTSC(n)         (0x44 + 4 * (n))

#define USBCMD_RS        (1u << 0)
#define USBCMD_HCRESET   (1u << 1)
#define USBCMD_PSE       (1u << 4)
#define USBCMD_ASE       (1u << 5)
#define USBCMD_IAAD      (1u << 6)

#define USBSTS_HCH       (1u << 12)
#define USBSTS_RECLAM    (1u << 13)
#define USBSTS_PSS       (1u << 14)
#define USBSTS_ASS       (1u << 15)

#define PORTSC_CCS       (1u << 0)   /* Current Connect Status */
#define PORTSC_PEC       (1u << 3)   /* Port Enable Change */
#define PORTSC_PE        (1u << 2)   /* Port Enabled */
#define PORTSC_PR        (1u << 8)   /* Port Reset */
#define PORTSC_LS_MASK   (3u << 10)  /* Line Status (1 = K-state = low-speed) */
#define PORTSC_PP        (1u << 12)  /* Port Power */
#define PORTSC_OWNER     (1u << 13)  /* Hand off to companion controller */

/* Extended capability IDs. */
#define EECP_USBLEGSUP   0x01

/* QH (Queue Head). The overlay area (cur_qtd..bufp[4]) is where the
 * HC copies the current qTD's state as it executes; it's also where
 * we initially seed next_qtd to point at the first qTD in our chain.
 * Layout per EHCI spec §3.6. */
struct ehci_qh {
    uint32_t hlp;                     /* horizontal link pointer */
    uint32_t ep_chars;                /* endpoint characteristics */
    uint32_t ep_caps;                 /* endpoint capabilities */
    uint32_t cur_qtd;
    /* ---- Overlay (HC writes through these as it executes) ---- */
    uint32_t next_qtd;
    uint32_t alt_qtd;
    uint32_t token;
    uint32_t bufp[5];
    uint32_t pad[3];                  /* round to 64 B for alignment ease */
} __attribute__((aligned(32)));

/* qTD (Queue Transfer Descriptor). Each qTD describes one transfer
 * (or a chunk of one). The HC walks next_qtd to step through a chain. */
struct ehci_qtd {
    uint32_t next_qtd;                /* phys ptr | T(bit 0) */
    uint32_t alt_qtd;                 /* alt pointer on short packet */
    uint32_t token;                   /* status + length + PID + toggle */
    uint32_t bufp[5];                 /* page pointers; bufp[0] has offset */
    /* No padding required; 32 bytes is already 32-aligned. */
} __attribute__((aligned(32)));

/* qTD token bit layout. */
#define QTD_TOK_PINGE     (1u << 0)
#define QTD_TOK_SPLITX    (1u << 1)
#define QTD_TOK_MISSED    (1u << 2)
#define QTD_TOK_XACTERR   (1u << 3)
#define QTD_TOK_BABBLE    (1u << 4)
#define QTD_TOK_BUFERR    (1u << 5)
#define QTD_TOK_HALTED    (1u << 6)
#define QTD_TOK_ACTIVE    (1u << 7)
#define QTD_TOK_PID_OUT   (0u << 8)
#define QTD_TOK_PID_IN    (1u << 8)
#define QTD_TOK_PID_SETUP (2u << 8)
#define QTD_TOK_CERR_3    (3u << 10)
#define QTD_TOK_LEN_SHIFT 16
#define QTD_TOK_DT        (1u << 31)
/* Errors that mean we have to give up. */
#define QTD_TOK_ERRORS    (QTD_TOK_XACTERR | QTD_TOK_BABBLE | \
                           QTD_TOK_BUFERR  | QTD_TOK_HALTED)

#define QH_HLP_TYP_QH     (1u << 1)
#define QTD_T             (1u << 0)
/* Endpoint Characteristics field bits. */
#define EPC_ADDR(a)       ((a) & 0x7Fu)
#define EPC_EP(e)         (((e) & 0xFu) << 8)
#define EPC_EPS_HS        (2u << 12)
#define EPC_DTC           (1u << 14)        /* toggle from qTD */
#define EPC_H             (1u << 15)        /* head of reclamation */
#define EPC_MAX(m)        (((m) & 0x7FFu) << 16)
#define EPC_C             (1u << 27)        /* control endpoint flag — full/low only */

/* Per-controller state. */
struct ehci {
    int                 in_use;
    struct pci_device   pci;
    volatile uint8_t   *bar0;
    volatile uint8_t   *op;            /* bar0 + CAPLENGTH */
    uint32_t            n_ports;
    uint32_t            eecp_off;
    struct ehci_qh     *async_head;
};

static struct ehci g_ehci;

int ehci_present(void) { return g_ehci.in_use; }

/* ---- MMIO helpers ---------------------------------------------- */

static inline uint8_t  cap_r8 (uint32_t off) {
    return *(volatile uint8_t  *)(g_ehci.bar0 + off);
}
static inline uint16_t cap_r16(uint32_t off) {
    return *(volatile uint16_t *)(g_ehci.bar0 + off);
}
static inline uint32_t cap_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_ehci.bar0 + off);
}
static inline uint32_t op_r32 (uint32_t off) {
    return *(volatile uint32_t *)(g_ehci.op + off);
}
static inline void     op_w32 (uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_ehci.op + off) = v;
}

/* PCI config-byte helper at arbitrary offset. */
static uint8_t pci_cfg_r8(struct pci_device *p, uint8_t off) {
    return pci_config_read8(p->bus, p->device, p->func, off);
}
static uint32_t pci_cfg_r32(struct pci_device *p, uint8_t off) {
    return pci_config_read32(p->bus, p->device, p->func, off);
}
static void pci_cfg_w32(struct pci_device *p, uint8_t off, uint32_t v) {
    pci_config_write32(p->bus, p->device, p->func, off, v);
}

/* ---- BIOS handoff --------------------------------------------- */

static int bios_handoff(void) {
    if (g_ehci.eecp_off < 0x40) return 0;     /* no extended caps */
    /* Walk the cap list. Each cap is a 32-bit DWORD where:
     *   [7:0]   cap ID
     *   [15:8]  next ptr (PCI config offset; 0 = end)
     * The USBLEGSUP cap has:
     *   [16]    HC BIOS Owned Semaphore
     *   [24]    HC OS Owned Semaphore
     * Write 1 to bit 24, poll until bit 16 clears. */
    uint8_t off = (uint8_t)g_ehci.eecp_off;
    for (int hop = 0; hop < 16 && off; hop++) {
        uint32_t cap = pci_cfg_r32(&g_ehci.pci, off);
        uint8_t  id  = (uint8_t)(cap & 0xFFu);
        uint8_t  nxt = (uint8_t)((cap >> 8) & 0xFFu);
        if (id == EECP_USBLEGSUP) {
            /* Set OS-owned semaphore at bit 24. */
            pci_cfg_w32(&g_ehci.pci, off, cap | (1u << 24));
            /* Wait up to ~1 s for BIOS-owned bit to clear. */
            uint32_t start = pit_ticks();
            for (;;) {
                cap = pci_cfg_r32(&g_ehci.pci, off);
                if (!(cap & (1u << 16))) break;
                if (pit_ticks() - start > 100) {
                    kprintf("ehci: BIOS handoff timed out (cap=0x%x) — forcing\n", cap);
                    /* Some BIOSes never release. Forcibly clear bit 16
                     * and proceed; QEMU never owns it in the first place. */
                    pci_cfg_w32(&g_ehci.pci, off,
                                (cap | (1u << 24)) & ~(1u << 16));
                    break;
                }
            }
            return 0;
        }
        off = nxt;
    }
    return 0;
}

/* ---- bring-up ------------------------------------------------- */

static void *alloc_page(void) {
    /* 4 KiB-aligned, zeroed. */
    uint8_t *raw = (uint8_t *)kmalloc(8192);
    if (!raw) return 0;
    uintptr_t a = ((uintptr_t)raw + 0xFFFu) & ~(uintptr_t)0xFFFu;
    memset((void *)a, 0, 4096);
    return (void *)a;
}

static int find_ehci_pci(struct pci_device *out) {
    for (int i = 0; g_known[i].vendor; i++) {
        if (pci_find(g_known[i].vendor, g_known[i].device, out) == 0) {
            kprintf("ehci: PCI %u:%u.%u  %s (0x%x:0x%x)\n",
                    out->bus, out->device, out->func,
                    g_known[i].name,
                    g_known[i].vendor, g_known[i].device);
            return 0;
        }
    }
    return -1;
}

int ehci_init(void) {
    if (g_ehci.in_use) return 0;

    if (find_ehci_pci(&g_ehci.pci) != 0) {
        return -1;     /* silent: no EHCI in this QEMU setup */
    }

    /* Enable MMIO + bus master. */
    uint32_t cmd_sts = pci_config_read32(g_ehci.pci.bus, g_ehci.pci.device,
                                         g_ehci.pci.func, PCI_CFG_COMMAND);
    pci_config_write32(g_ehci.pci.bus, g_ehci.pci.device, g_ehci.pci.func,
                       PCI_CFG_COMMAND, cmd_sts | 0x6u);

    uint32_t bar0 = pci_config_read32(g_ehci.pci.bus, g_ehci.pci.device,
                                      g_ehci.pci.func, PCI_CFG_BAR0);
    if (bar0 & 1u) {
        kprintf("ehci: BAR0 is I/O space (0x%x) — expected MMIO\n", bar0);
        return -1;
    }
    uintptr_t mmio = (uintptr_t)(bar0 & ~0xFu);
    if (paging_map(mmio, mmio, PTE_PRESENT | PTE_WRITABLE) < 0) {
        kprintf("ehci: paging_map(0x%x) failed\n", (unsigned)mmio);
        return -1;
    }
    g_ehci.bar0 = (volatile uint8_t *)mmio;

    uint8_t  caplen   = cap_r8 (CAP_CAPLENGTH);
    uint16_t hciver   = cap_r16(CAP_HCIVERSION);
    uint32_t hcsparams = cap_r32(CAP_HCSPARAMS);
    uint32_t hccparams = cap_r32(CAP_HCCPARAMS);
    g_ehci.op       = g_ehci.bar0 + caplen;
    g_ehci.n_ports  = hcsparams & 0xFu;
    g_ehci.eecp_off = (hccparams >> 8) & 0xFFu;
    kprintf("ehci: caplen=%u  hciver=0x%x  ports=%u  eecp=0x%x\n",
            caplen, hciver, g_ehci.n_ports, g_ehci.eecp_off);

    /* Handle BIOS handoff. On QEMU this is a no-op (no BIOS owns it). */
    bios_handoff();

    /* Stop the controller if it's running. */
    op_w32(OP_USBCMD, 0);
    {
        uint32_t start = pit_ticks();
        while (!(op_r32(OP_USBSTS) & USBSTS_HCH)) {
            if (pit_ticks() - start > 100) {
                kprintf("ehci: HC didn't halt — USBSTS=0x%x\n",
                        op_r32(OP_USBSTS));
                return -1;
            }
        }
    }

    /* Reset. The HC clears HCRESET when done. */
    op_w32(OP_USBCMD, USBCMD_HCRESET);
    {
        uint32_t start = pit_ticks();
        while (op_r32(OP_USBCMD) & USBCMD_HCRESET) {
            if (pit_ticks() - start > 100) {
                kprintf("ehci: HC reset stuck — USBCMD=0x%x\n",
                        op_r32(OP_USBCMD));
                return -1;
            }
        }
    }

    /* Allocate the async placeholder QH — the head of our (empty)
     * async list. H bit set so the controller knows it's the head;
     * QH halted so it never tries to execute transfers. */
    g_ehci.async_head = (struct ehci_qh *)alloc_page();
    if (!g_ehci.async_head) {
        kprintf("ehci: async QH alloc failed\n");
        return -1;
    }
    uint32_t qh_phys = (uint32_t)(uintptr_t)g_ehci.async_head;
    /* Horizontal Link Pointer: self-loop (next QH = ourselves).
     * Bits [4:0]: T=0 (not terminate), Typ=01 (QH), reserved. */
    g_ehci.async_head->hlp      = qh_phys | (1u << 1);
    /* Endpoint Characteristics: bit 15 = H (head of reclamation list).
     * Max packet = 0 (no transfers); EPS = 0 (full-speed dummy). */
    g_ehci.async_head->ep_chars = (1u << 15);
    g_ehci.async_head->ep_caps  = 0;
    /* Mark the qTD list as terminated and the QH as halted. */
    g_ehci.async_head->cur_qtd  = 0;
    g_ehci.async_head->next_qtd = 1;    /* T bit */
    g_ehci.async_head->alt_qtd  = 1;
    g_ehci.async_head->token    = (1u << 6);   /* Halted */

    /* PERIODICLISTBASE / ASYNCLISTADDR. We only enable the async list. */
    op_w32(OP_CTRLDSSEGMENT,    0);
    op_w32(OP_PERIODICLISTBASE, 0);
    op_w32(OP_ASYNCLISTADDR,    qh_phys);

    /* Interrupt threshold = 8 microframes (default); Run/Stop + Async
     * schedule enable. We don't enable the periodic schedule because
     * we haven't built a frame list. */
    op_w32(OP_USBCMD, USBCMD_RS | USBCMD_ASE | (8u << 16));

    /* Confirm RS took: USBSTS.HCH should clear. */
    {
        uint32_t start = pit_ticks();
        while (op_r32(OP_USBSTS) & USBSTS_HCH) {
            if (pit_ticks() - start > 100) {
                kprintf("ehci: HC didn't start running — USBSTS=0x%x\n",
                        op_r32(OP_USBSTS));
                return -1;
            }
        }
    }

    /* Route root-hub ports to EHCI (away from any companion). */
    op_w32(OP_CONFIGFLAG, 1);

    /* Survey ports. Power them on first (the PP bit may have come up
     * clear after reset), then read CCS / LS / Owner. */
    for (uint32_t i = 0; i < g_ehci.n_ports; i++) {
        uint32_t pscs = op_r32(OP_PORTSC(i));
        /* Some EHCI variants require explicit power-on; on QEMU PPC=0
         * (always powered) so this is a no-op there. */
        if (!(pscs & PORTSC_PP)) {
            op_w32(OP_PORTSC(i), pscs | PORTSC_PP);
            pit_sleep(20);
            pscs = op_r32(OP_PORTSC(i));
        }
        if (pscs & PORTSC_CCS) {
            /* Connected. If the line state is K (LS_MASK == 0x1 << 10),
             * it's a low-speed device — hand off to companion. */
            int low_speed = ((pscs >> 10) & 0x3) == 0x1;
            if (low_speed) {
                kprintf("ehci: port %u  low-speed device — releasing to companion\n", i);
                op_w32(OP_PORTSC(i), pscs | PORTSC_OWNER);
            } else {
                /* Reset to enable the port (high-speed devices need an
                 * explicit reset to become enabled). */
                op_w32(OP_PORTSC(i), (pscs & ~PORTSC_PE) | PORTSC_PR);
                pit_sleep(50);
                op_w32(OP_PORTSC(i), pscs & ~PORTSC_PR);
                pit_sleep(20);
                pscs = op_r32(OP_PORTSC(i));
                if (pscs & PORTSC_PE) {
                    kprintf("ehci: port %u  high-speed device enabled (PORTSC=0x%x)\n",
                            i, pscs);
                } else {
                    /* Failed to enable as high-speed → also a full-/
                     * low-speed device; hand off. */
                    kprintf("ehci: port %u  not high-speed (PORTSC=0x%x) — releasing\n",
                            i, pscs);
                    op_w32(OP_PORTSC(i), pscs | PORTSC_OWNER);
                }
            }
        }
    }

    g_ehci.in_use = 1;
    kprintf("ehci: controller running (async list @ 0x%x, %u ports)\n",
            qh_phys, g_ehci.n_ports);
    return 0;
}

void ehci_probe_ports(int *connected, int *n_ports) {
    int max = *n_ports;
    int n   = (int)g_ehci.n_ports;
    if (n > max) n = max;
    if (!g_ehci.in_use) { *n_ports = 0; return; }
    for (int i = 0; i < n; i++) {
        uint32_t pscs = op_r32(OP_PORTSC(i));
        /* Owned by us + connected + enabled = a high-speed device. */
        int owned = !(pscs & PORTSC_OWNER);
        connected[i] = (owned && (pscs & PORTSC_CCS) && (pscs & PORTSC_PE)) ? 1 : 0;
    }
    *n_ports = n;
}

/* ===================================================================
 * Transfer path — session 126: class drivers dispatch through
 * g_ehci_hc_ops when a device lives on the EHCI controller.
 * =================================================================== */

#include "usb_hc.h"
#include "kmalloc.h"
#include "spinlock.h"

static spinlock_t g_ehci_xfer_lock;

/* Allocate a 32-byte-aligned qTD, zero-initialized. Same kmalloc
 * over-allocate trick we use elsewhere. */
static struct ehci_qtd *alloc_qtd(void) {
    uint8_t *raw = (uint8_t *)kmalloc(sizeof(struct ehci_qtd) + 32);
    if (!raw) return 0;
    uintptr_t a = ((uintptr_t)raw + 31u) & ~31u;
    struct ehci_qtd *q = (struct ehci_qtd *)a;
    memset(q, 0, sizeof(*q));
    return q;
}

/* Same but for QH (32-byte alignment is enough for QHs too). */
static struct ehci_qh *alloc_qh_xfer(void) {
    uint8_t *raw = (uint8_t *)kmalloc(sizeof(struct ehci_qh) + 32);
    if (!raw) return 0;
    uintptr_t a = ((uintptr_t)raw + 31u) & ~31u;
    struct ehci_qh *q = (struct ehci_qh *)a;
    memset(q, 0, sizeof(*q));
    return q;
}

/* Fill in buffer pointers for a transfer. The first page pointer
 * has the in-page offset baked into bits [11:0]; subsequent ones are
 * page-aligned. Returns the next byte offset reached (= original len,
 * since we cap at 4 KiB per qTD and never split). */
static void qtd_set_buffer(struct ehci_qtd *q, void *buf, uint32_t len) {
    uintptr_t p = (uintptr_t)buf;
    q->bufp[0] = (uint32_t)p;
    /* For len ≤ 4 KiB we never cross a page boundary off bufp[0].
     * For up to 5 pages = 20 KiB we'd fill bufp[1..4]; the chunk
     * size in our callers is bounded so the simple form covers it. */
    for (int i = 1; i < 5; i++) {
        uintptr_t next = (p & ~0xFFFu) + (uint32_t)i * 4096u;
        q->bufp[i] = (uint32_t)next;
    }
    (void)len;
}

/* Build a single qTD. `toggle_bit` is the DT bit (already shifted to
 * its position) or 0. `pid` is one of QTD_TOK_PID_*. `next_phys` is
 * the next qTD's phys ptr or 1 (T) to terminate. */
static void build_qtd(struct ehci_qtd *q, uint32_t pid, uint32_t len,
                      uint32_t toggle_bit, void *buf, uint32_t next_phys)
{
    q->next_qtd = next_phys;
    q->alt_qtd  = QTD_T;
    q->token    = QTD_TOK_ACTIVE | QTD_TOK_CERR_3 | pid |
                  (len << QTD_TOK_LEN_SHIFT) | toggle_bit;
    if (buf && len) {
        qtd_set_buffer(q, buf, len);
    } else {
        for (int i = 0; i < 5; i++) q->bufp[i] = 0;
    }
}

/* Build a QH for an endpoint + run the qTD chain through the async
 * list. Returns 0 on success, -1 on timeout or error. */
static int run_async(uint8_t addr, int ep, int max_packet,
                     int is_control, struct ehci_qtd *first,
                     struct ehci_qtd *last)
{
    struct ehci_qh *qh = alloc_qh_xfer();
    if (!qh) return -1;

    qh->ep_chars = EPC_ADDR(addr) | EPC_EP(ep) | EPC_EPS_HS |
                   EPC_DTC | EPC_MAX(max_packet);
    if (is_control) qh->ep_chars |= EPC_C;
    qh->ep_caps  = (1u << 30);                 /* Mult = 1 */
    qh->cur_qtd  = 0;
    qh->next_qtd = (uint32_t)(uintptr_t)first;  /* point at SETUP/first */
    qh->alt_qtd  = QTD_T;
    qh->token    = 0;                            /* not halted, not active */

    /* Link QH into the async list right after async_head. */
    spin_lock(&g_ehci_xfer_lock);
    uint32_t qh_phys = (uint32_t)(uintptr_t)qh;
    qh->hlp = g_ehci.async_head->hlp;
    g_ehci.async_head->hlp = qh_phys | QH_HLP_TYP_QH;

    /* Wait for the last qTD's Active bit to clear or an error bit
     * to set. Deadline: 5 s. */
    uint32_t start = pit_ticks();
    int rc = 0;
    for (;;) {
        uint32_t tok = ((volatile uint32_t *)last)[2];   /* token */
        if (tok & QTD_TOK_ERRORS) { rc = -1; break; }
        if (!(tok & QTD_TOK_ACTIVE)) break;
        if (pit_ticks() - start > 500) { rc = -1; break; }
    }

    /* Unlink the QH from the async list. Bypass it: write our hlp
     * into async_head. Then do the IAAD doorbell handshake so we
     * know the HC has finished with the QH's memory before we free
     * (or reuse) it. */
    g_ehci.async_head->hlp = qh->hlp;
    op_w32(OP_USBCMD, op_r32(OP_USBCMD) | USBCMD_IAAD);
    uint32_t start2 = pit_ticks();
    while (!(op_r32(OP_USBSTS) & (1u << 5))) {   /* USBSTS.IAA bit 5 */
        if (pit_ticks() - start2 > 50) break;     /* QEMU is generous; bail anyway */
    }
    op_w32(OP_USBSTS, 1u << 5);                  /* clear IAA */
    spin_unlock(&g_ehci_xfer_lock);
    return rc;
}

/* ---- control transfer ---------------------------------------------- */

static int ehci_control_transfer(uint8_t addr, int low_speed, int ep0_max,
                                 const struct usb_setup_packet *setup,
                                 void *data, int data_len, int data_in)
{
    (void)low_speed;     /* EHCI handles high-speed only */
    if (ep0_max <= 0) ep0_max = 64;
    if (!g_ehci.in_use) return USB_ERR_OTHER;

    /* Stage 1: SETUP qTD. Always 8 bytes, PID=SETUP, toggle=0. */
    struct ehci_qtd *q_setup  = alloc_qtd();
    struct ehci_qtd *q_data   = (data_len > 0) ? alloc_qtd() : 0;
    struct ehci_qtd *q_status = alloc_qtd();
    if (!q_setup || !q_status || (data_len > 0 && !q_data))
        return USB_ERR_OTHER;

    /* Copy the setup packet into a kmalloc'd buffer; the caller's may
     * be on a transient stack page. */
    struct usb_setup_packet *sbuf =
        (struct usb_setup_packet *)kmalloc(sizeof(*sbuf));
    if (!sbuf) return USB_ERR_OTHER;
    *sbuf = *setup;

    struct ehci_qtd *first = q_setup;
    struct ehci_qtd *last  = q_status;

    /* Status qTD: opposite-direction zero-length, toggle=1 (DATA1). */
    uint32_t status_pid = data_in ? QTD_TOK_PID_OUT : QTD_TOK_PID_IN;
    build_qtd(q_status, status_pid, 0, QTD_TOK_DT, 0, QTD_T);

    if (q_data) {
        uint32_t data_pid = data_in ? QTD_TOK_PID_IN : QTD_TOK_PID_OUT;
        build_qtd(q_data, data_pid, (uint32_t)data_len, QTD_TOK_DT,
                  data, (uint32_t)(uintptr_t)q_status);
        build_qtd(q_setup, QTD_TOK_PID_SETUP, 8, 0,
                  sbuf, (uint32_t)(uintptr_t)q_data);
    } else {
        build_qtd(q_setup, QTD_TOK_PID_SETUP, 8, 0,
                  sbuf, (uint32_t)(uintptr_t)q_status);
    }

    int rc = run_async(addr, 0, ep0_max, /*is_control=*/1, first, last);
    return rc == 0 ? USB_OK : USB_ERR_OTHER;
}

/* ---- bulk + int (single chunk in flight) --------------------------- */

static int ehci_bulk_xfer(uint32_t pid, uint8_t addr, int ep_max, int ep,
                          void *buf, int len, int *toggle)
{
    if (!g_ehci.in_use) return USB_ERR_OTHER;
    if (len <= 0 || len > 16384) return USB_ERR_OTHER;

    /* One qTD covers up to 5 pages — far more than our callers ever
     * pass. Single qTD with EOT in next_qtd. */
    struct ehci_qtd *q = alloc_qtd();
    if (!q) return USB_ERR_OTHER;
    uint32_t dt = (*toggle & 1) ? QTD_TOK_DT : 0;
    build_qtd(q, pid, (uint32_t)len, dt, buf, QTD_T);

    int rc = run_async(addr, ep, ep_max, /*is_control=*/0, q, q);
    if (rc != 0) return USB_ERR_OTHER;

    /* qTD token's bytes-to-transfer field shows residual bytes
     * remaining (0 if all done; non-zero on a short packet). Flip the
     * toggle for the caller based on the number of max-packet-sized
     * chunks consumed. Simpler: count chunks ceil(len / ep_max) and
     * flip toggle if odd. */
    int chunks = (len + ep_max - 1) / ep_max;
    if (chunks & 1) *toggle = (*toggle ^ 1);

    uint32_t remaining = (q->token >> QTD_TOK_LEN_SHIFT) & 0x7FFFu;
    return (int)((uint32_t)len - remaining);
}

static int ehci_bulk_in(uint8_t addr, int ep_max, int ep,
                        void *buf, int len, int *toggle) {
    return ehci_bulk_xfer(QTD_TOK_PID_IN, addr, ep_max, ep,
                          buf, len, toggle);
}
static int ehci_bulk_out(uint8_t addr, int ep_max, int ep,
                         const void *buf, int len, int *toggle) {
    return ehci_bulk_xfer(QTD_TOK_PID_OUT, addr, ep_max, ep,
                          (void *)buf, len, toggle);
}

/* int_in: HID polling. Same code path as bulk_in for our purposes —
 * the class driver does the timing. The (low_speed, ...) signature
 * matches uhci's for vtable compatibility; low-speed devices are on
 * the companion controller so we shouldn't see them here. */
static int ehci_int_in(uint8_t addr, int low_speed, int ep_max,
                       int ep, void *buf, int max_len, int *toggle)
{
    (void)low_speed;
    return ehci_bulk_xfer(QTD_TOK_PID_IN, addr, ep_max, ep,
                          buf, max_len, toggle);
}

/* Exposed vtable. */
const struct usb_hc_ops g_ehci_hc_ops = {
    .control_transfer = ehci_control_transfer,
    .int_in           = ehci_int_in,
    .bulk_in          = ehci_bulk_in,
    .bulk_out         = ehci_bulk_out,
};
