#include "ac97.h"
#include "pci.h"
#include "pmm.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"
#include "../include/io.h"

/* ---- AC97 register map ------------------------------------------ */

/* NAM (Native Audio Mixer) registers — offsets from BAR0. */
#define NAM_RESET            0x00    /* write any value resets codec */
#define NAM_MASTER_VOL       0x02    /* word: bit15=mute, 5b L att, 5b R att */
#define NAM_PCM_OUT_VOL      0x18    /* same shape as master */
#define NAM_PCM_FRONT_DAC_RATE 0x2C  /* word: sample rate (Hz) */

/* NABM (Native Audio Bus Master) registers — offsets from BAR1.
 * The PCM-Out (PO) box is at 0x10..0x1B; we don't use input or mic. */
#define NABM_PO_BDBAR        0x10    /* dword: BDL base addr (phys, 8B aligned) */
#define NABM_PO_CIV          0x14    /* byte:  current index value (RO) */
#define NABM_PO_LVI          0x15    /* byte:  last valid index */
#define NABM_PO_SR           0x16    /* word:  status register */
#define NABM_PO_PICB         0x18    /* word:  position in current buffer */
#define NABM_PO_PIV          0x1A    /* byte:  prefetched index value (RO) */
#define NABM_PO_CR           0x1B    /* byte:  control */

#define NABM_GLOB_CNT        0x2C
#define NABM_GLOB_STA        0x30

/* CR bits (PO control register). */
#define CR_RPBM              0x01    /* run/pause bus master (1=run) */
#define CR_RR                0x02    /* reset registers */
#define CR_LVBIE             0x04    /* last valid buffer interrupt enable */
#define CR_FEIE              0x08    /* fifo error interrupt enable */
#define CR_IOCE              0x10    /* interrupt on completion enable */

/* SR bits (PO status). */
#define SR_DCH               0x01    /* DMA controller halted */
#define SR_CELV              0x02    /* current equals last valid */
#define SR_LVBCI             0x04    /* last valid buffer completion intr */
#define SR_BCIS              0x08    /* buffer completion intr status */
#define SR_FIFOE             0x10    /* FIFO error */

/* GLOB_CNT bits. */
#define GLOB_CNT_GIE         0x01
#define GLOB_CNT_COLD_RESET  0x02
#define GLOB_CNT_WARM_RESET  0x04

/* ---- BDL layout ------------------------------------------------- */

/* AC97 buffer descriptor entry — 8 bytes.
 *   addr        : 32-bit physical address of PCM data
 *   samples     : 16-bit count of 16-BIT SAMPLES (NOT bytes, NOT
 *                 stereo-frames). For stereo 16-bit PCM, this is
 *                 (byte_len / 2). Max 0xFFFF.
 *   flags_low   : low byte of control bits (we leave 0)
 *   flags_high  : high byte: bit 15 = IOC, bit 14 = BUP. We set
 *                 IOC on every buffer to fire IRQ on completion. */
struct ac97_bdle {
    uint32_t addr;
    uint16_t samples;
    uint16_t ctl;
} __attribute__((packed));

/* The AC97 hardware BDL has a fixed 32-entry ring (CIV is a 5-bit
 * register). To avoid silent staging-page reuse while DMA is mid-
 * read, give each BDL slot its own staging page — 32 × 4 KiB =
 * 128 KiB of kernel memory. Acceptable on a 32-MiB system and the
 * code is much simpler than juggling fewer pages.
 *
 * 32 slots × 4 KiB / 192000 B/s ≈ 680 ms of staged audio. Anything
 * longer just blocks ac97_play in the slot-recycle wait until the
 * DMA passes a slot we want to reuse. */
#define BDL_NUM_ENTRIES   32
#define STAGING_NUM_PAGES 32

/* ---- driver state ----------------------------------------------- */

static int      g_initialized;
static struct pci_device g_pci;
static struct ac97_bdle *g_bdl;            /* kernel-virt = phys (identity map) */
static uint8_t  *g_stage[STAGING_NUM_PAGES]; /* per-BDL-slot staging buffers */
static int      g_next_slot;               /* round-robin BDL writer index */
static int      g_slot_in_use[BDL_NUM_ENTRIES];

/* I/O helpers: read/write NAM (BAR0 + offset) or NABM (BAR1 + offset). */
static uint16_t nam_r16(uint8_t off) { return inw(g_pci.io_base + off); }
static void     nam_w16(uint8_t off, uint16_t v) { outw(g_pci.io_base + off, v); }

static uint8_t  nabm_r8 (uint8_t off) { return inb(g_pci.io_base1 + off); }
static uint16_t nabm_r16(uint8_t off) { return inw(g_pci.io_base1 + off); }
static uint32_t nabm_r32(uint8_t off) { return inl(g_pci.io_base1 + off); }
static void     nabm_w8 (uint8_t off, uint8_t  v) { outb(g_pci.io_base1 + off, v); }
static void     nabm_w32(uint8_t off, uint32_t v) { outl(g_pci.io_base1 + off, v); }

/* ---- init -------------------------------------------------------- */

void ac97_init(void) {
    if (g_initialized) return;

    /* Find Intel AC97 (vendor 0x8086, device 0x2415). */
    if (pci_find(0x8086, 0x2415, &g_pci) != 0) {
        kprintf("ac97: no Intel AC97 controller (vendor 0x8086 dev 0x2415) "
                "— audio disabled (boot QEMU with `-device AC97`)\n");
        return;
    }

    kprintf("ac97: found at PCI %u:%u.%u  NAM=0x%x  NABM=0x%x  IRQ=%u\n",
            (unsigned)g_pci.bus, (unsigned)g_pci.device, (unsigned)g_pci.func,
            (unsigned)g_pci.io_base, (unsigned)g_pci.io_base1,
            (unsigned)g_pci.irq_line);

    /* === Cold reset the codec via NABM GLOB_CNT === */
    nabm_w32(NABM_GLOB_CNT, GLOB_CNT_COLD_RESET);
    /* Wait for codec ready. The AC'97 spec gives the codec ~600µs;
     * we poll GLOB_STA bit 8 (Primary Codec Ready) with a 50ms cap.
     * QEMU's emulation reports ready almost immediately. */
    int waited = 0;
    while (waited < 50) {
        uint32_t sta = nabm_r32(NABM_GLOB_STA);
        if (sta & 0x100) break;        /* PCR — Primary Codec Ready */
        pit_sleep(1);
        waited++;
    }

    /* === Reset the codec mixer via NAM === */
    nam_w16(NAM_RESET, 1);

    /* === Configure mixer: full volume, unmuted === */
    /* Master volume: bit 15 = mute, bits 0-4 = right att (0=max), bits 8-12 = left.
     * Set both attenuations to 0 (loudest). */
    nam_w16(NAM_MASTER_VOL, 0x0000);
    nam_w16(NAM_PCM_OUT_VOL, 0x0000);

    /* === Set sample rate. Some codecs default to 48kHz; ours requires
     * VRA (variable rate audio) to set arbitrary rates. We just write
     * 48000 — for codecs without VRA the write is silently ignored
     * and 48 kHz is the default anyway. */
    nam_w16(NAM_PCM_FRONT_DAC_RATE, AC97_SAMPLE_RATE);

    /* === Allocate BDL — must be physically contiguous, 8-byte aligned.
     * We use one PMM page (4 KiB), holding 32 × 8-byte entries. */
    g_bdl = (struct ac97_bdle *)pmm_alloc_page();
    if (!g_bdl) {
        kputs("ac97: BDL alloc failed\n");
        return;
    }
    memset(g_bdl, 0, 4096);

    /* === Allocate staging buffers — one PMM page per BDL slot we'll
     * actively use. Beyond STAGING_NUM_PAGES, ac97_play wraps. */
    for (int i = 0; i < STAGING_NUM_PAGES; i++) {
        g_stage[i] = (uint8_t *)pmm_alloc_page();
        if (!g_stage[i]) {
            kputs("ac97: staging buffer alloc failed\n");
            return;
        }
        memset(g_stage[i], 0, 4096);
    }

    /* === Reset the PCM-Out box (CR.RR = 1 self-clears). === */
    nabm_w8(NABM_PO_CR, CR_RR);
    waited = 0;
    while (waited < 50 && (nabm_r8(NABM_PO_CR) & CR_RR)) {
        pit_sleep(1);
        waited++;
    }

    /* === Install BDL base. Physical address = virtual (identity-mapped). */
    nabm_w32(NABM_PO_BDBAR, (uint32_t)(uintptr_t)g_bdl);

    /* CR is left at 0 (paused) — DMA starts when the first ac97_play
     * call queues data and sets CR.RPBM. */

    g_initialized = 1;
    g_next_slot = 0;
    for (int i = 0; i < BDL_NUM_ENTRIES; i++) g_slot_in_use[i] = 0;

    kprintf("ac97: codec ready, %d KiB staging, BDL @ phys 0x%x\n",
            STAGING_NUM_PAGES * 4, (unsigned)(uintptr_t)g_bdl);
}

int ac97_available(void) { return g_initialized; }

/* ---- play -------------------------------------------------------- */

int ac97_play(const void *pcm, int n) {
    if (!g_initialized) return -1;
    if (n <= 0 || (n & 3)) return -1;     /* must be stereo 16-bit aligned */

    /* Strategy: stop the DMA, queue everything into BDL slots, set
     * LVI to the last filled slot, start DMA. Then return.
     *
     * Why stop-then-restart instead of pipelining: the AC97 engine
     * sets the DCH (DMA-controller-halted) status bit the instant
     * CIV advances past LVI, and on QEMU restarting via just an
     * LVI bump after DCH is sticky doesn't reliably resume play.
     * The clean way is to fully reset the PCM-Out box (CR.RR self-
     * clears) before each batch.
     *
     * Cost: there's a sub-millisecond gap between consecutive
     * ac97_play calls. For the tones beep.c plays this is
     * inaudible; long continuous streams would want a per-IRQ
     * pipelined refiller. */
    const uint8_t *src = (const uint8_t *)pcm;

    /* Reset and re-arm. */
    nabm_w8(NABM_PO_CR, CR_RR);
    int waited = 0;
    while (waited < 50 && (nabm_r8(NABM_PO_CR) & CR_RR)) {
        pit_sleep(1); waited++;
    }
    nabm_w32(NABM_PO_BDBAR, (uint32_t)(uintptr_t)g_bdl);

    int total_queued = 0;
    int slot = 0;
    while (total_queued < n && slot < BDL_NUM_ENTRIES) {
        int chunk = n - total_queued;
        if (chunk > 4096) chunk = 4096;

        uint8_t *stage = g_stage[slot];
        for (int i = 0; i < chunk; i++) stage[i] = src[total_queued + i];

        /* samples count = bytes / 2 (16-bit-word count, NOT
         * stereo-frame count, NOT byte count). */
        g_bdl[slot].addr    = (uint32_t)(uintptr_t)stage;
        g_bdl[slot].samples = (uint16_t)(chunk / 2);
        g_bdl[slot].ctl     = 0;
        total_queued += chunk;
        slot++;
    }

    if (slot == 0) return 0;     /* nothing to do (n=0 was rejected above) */

    /* LVI = index of last valid entry (= slot - 1 since slot
     * post-incremented). DMA plays CIV..LVI inclusive then halts. */
    nabm_w8(NABM_PO_LVI, (uint8_t)(slot - 1));
    nabm_w8(NABM_PO_CR,  CR_RPBM);

    return total_queued;
}

void ac97_stop(void) {
    if (!g_initialized) return;
    nabm_w8(NABM_PO_CR, 0);              /* clear RUN */
    /* Wait for DMA to halt. */
    int waited = 0;
    while (waited < 100 && !(nabm_r16(NABM_PO_SR) & SR_DCH)) {
        pit_sleep(1);
        waited++;
    }
    /* Reset slot tracker so the next play starts fresh. */
    g_next_slot = 0;
    for (int i = 0; i < BDL_NUM_ENTRIES; i++) g_slot_in_use[i] = 0;
}
