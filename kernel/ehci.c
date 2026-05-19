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

/* QH (Queue Head) — 48 bytes total but we keep ours empty/HALTED.
 * Layout per EHCI spec §3.6. */
struct ehci_qh {
    uint32_t hlp;                     /* horizontal link pointer */
    uint32_t ep_chars;                /* endpoint characteristics */
    uint32_t ep_caps;                 /* endpoint capabilities */
    uint32_t cur_qtd;
    uint32_t next_qtd;
    uint32_t alt_qtd;
    uint32_t token;                   /* status byte etc. */
    uint32_t bufp[5];
    uint32_t pad;                     /* round to 64 B for alignment ease */
} __attribute__((aligned(32)));

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
    kprintf("ehci: NOTE — class-driver transfer integration is a follow-up;\n"
            "      USB devices on the EHCI ports will surface through the\n"
            "      companion UHCI controller (if present) for now.\n");
    return 0;
}
