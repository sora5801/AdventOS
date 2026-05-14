#include "lapic.h"
#include "paging.h"
#include "kprintf.h"
#include "../include/io.h"

/* MMIO accessors. Volatile so the compiler doesn't elide the loads. */
static volatile uint32_t *lapic_reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(LAPIC_VIRT_BASE + off);
}

static uint32_t lapic_read(uint32_t off) {
    return *lapic_reg(off);
}

static void lapic_write(uint32_t off, uint32_t val) {
    *lapic_reg(off) = val;
}

/* Map the LAPIC MMIO page into the kernel's PD. Both BSP and APs
 * see this mapping because it's installed in the global PD before
 * any AP boots. RW + present, no user, no PCD/PWT bits — the chipset
 * decodes the access by physical address regardless. */
static int g_mapped = 0;

/* Session 68: force the chipset into PIC mode via IMCR.
 *
 * IMCR (Interrupt Mode Configuration Register) is a legacy chipset
 * register that selects how the 8259 PIC's INT output is routed:
 *   IMCR=0 (PIC mode):       8259 INT  →  LAPIC LINT0 (legacy)
 *   IMCR=1 (Symmetric I/O):  8259 INT  →  disconnected; IOAPIC routes
 *
 * SeaBIOS in QEMU 10.x leaves IMCR=1 by default, expecting modern
 * OSes to drive the IOAPIC. Our kernel still uses the legacy 8259
 * for IRQ delivery, so we have to flip IMCR back to 0 before the
 * 8259 INT line will reach our CPU through the LAPIC. Symptom of
 * forgetting this: PIT/keyboard/COM1 IRQs all go silent the moment
 * lapic_enable() flips SVR.bit8 — exactly what session 68 hit.
 *
 * IMCR is accessed through two I/O ports:
 *   outb(0x22, 0x70)         select IMCR
 *   outb(0x23, mode)         0 = PIC, 1 = Symmetric I/O
 *
 * Safe even if the chipset doesn't implement IMCR — those ports
 * usually decode to nothing, the write just falls on the floor. */
static void imcr_select_pic_mode(void) {
    outb(0x22, 0x70);
    outb(0x23, 0x00);
}

void lapic_init(void) {
    if (g_mapped) return;
    if (paging_map(LAPIC_VIRT_BASE, LAPIC_PHYS_BASE,
                   PTE_PRESENT | PTE_WRITABLE) < 0) {
        kputs("lapic: failed to map MMIO\n");
        return;
    }
    g_mapped = 1;
    imcr_select_pic_mode();     /* MUST come before lapic_enable */
    lapic_enable();
    kprintf("lapic: BSP enabled, ID=%u version=0x%x\n",
            (unsigned)lapic_id(),
            (unsigned)(lapic_read(LAPIC_REG_VERSION) & 0xFF));
}

void lapic_enable(void) {
    /* Mask the LAPIC timer and error LVTs — we don't use them on BSP. */
    lapic_write(LAPIC_REG_LVT_TIMER, 0x10000u);   /* mask */
    lapic_write(LAPIC_REG_LVT_ERROR, 0x10000u);
    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_ESR, 0);

    /* Program LINT0/LINT1 for completeness — these matter only when
     * the LAPIC is software-enabled (SVR.bit8 = 1). On BSP we leave
     * the LAPIC software-disabled (see commentary below), so these
     * writes are effectively a no-op there. APs that bring up their
     * own LAPIC inherit these values from BSP's earlier write. */
    lapic_write(LAPIC_REG_LVT_LINT0, 0x700u);   /* ExtINT, unmasked */
    lapic_write(LAPIC_REG_LVT_LINT1, 0x400u);   /* NMI,    unmasked */

    /* Session 68 — DO NOT set SVR.bit8 here.
     *
     * Pre-session-68 this wrote `LAPIC_SVR_ENABLE | 0xFF`, software-
     * enabling the LAPIC on BSP. Under QEMU 10.x's i440fx chipset
     * emulation that has a fatal side effect: the moment SVR.bit8
     * goes high, the chipset stops routing the 8259 INT line to
     * LAPIC LINT0 (it expects the OS to be in symmetric-I/O mode
     * and driving the IOAPIC instead). Result: PIT IRQ 0, PS/2 IRQ
     * 1, COM1 IRQ 4 all go silent the instant SVR is written —
     * confirmed by tracing in this session.
     *
     * For -smp 1 we don't need LAPIC software-enable on BSP — BSP
     * uses PIT for ticks, doesn't send IPIs, doesn't run a LAPIC
     * timer. Leaving SVR.bit8 = 0 keeps the legacy 8259 → ExtINT →
     * CPU path alive, which is what every other piece of our IRQ
     * wiring (pic_send_eoi, IRQ stubs, mask register writes) assumes.
     *
     * Multi-CPU support would need to revisit this: BSP must
     * software-enable its LAPIC to send INIT/SIPI IPIs to APs. The
     * right fix at that point is to program the IOAPIC redirection
     * table for IRQs 0-15 instead of relying on the 8259 → LINT0
     * connection, then EOI through the LAPIC instead of the 8259.
     * For -smp 1 we sidestep that whole project. */
    lapic_write(LAPIC_REG_SVR, 0xFF);             /* vec 0xFF, NOT enabled */
}

uint32_t lapic_id(void) {
    /* Top 8 bits of the ID register hold the APIC ID. */
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_eoi(void) {
    /* Any write ends the interrupt. */
    lapic_write(LAPIC_REG_EOI, 0);
}

/* Wait for the previous IPI to leave the delivery state. Bit 12 of
 * ICR_LOW (PENDING) clears once the IPI has been accepted. Without
 * this, back-to-back IPIs can collide. We bound the wait by an
 * iteration cap — spin forever risks deadlocking the boot if the
 * LAPIC didn't come up properly (which would be a separate bug
 * worth knowing about, not a hang). */
static void lapic_ipi_wait(void) {
    for (int i = 0; i < 1000000; i++) {
        if ((lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_PENDING) == 0) return;
        __asm__ volatile ("pause");
    }
    kprintf("lapic: ICR pending stuck — continuing anyway\n");
}

void lapic_send_init(uint32_t dest_apic_id) {
    /* Two-step assert/deassert — INIT IPI first asserts, then a
     * matching de-assert clears the level. Most modern boxes (including
     * QEMU) accept the assert-only form, but the spec says do both. */
    lapic_write(LAPIC_REG_ICR_HIGH, dest_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_DELIVERY_INIT |
                LAPIC_ICR_TRIGGER_LEVEL |
                LAPIC_ICR_LEVEL_ASSERT);
    lapic_ipi_wait();

    lapic_write(LAPIC_REG_ICR_HIGH, dest_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_DELIVERY_INIT |
                LAPIC_ICR_TRIGGER_LEVEL);
    lapic_ipi_wait();
}

void lapic_send_sipi(uint32_t dest_apic_id, uint32_t entry_phys) {
    /* SIPI vector is `entry_phys >> 12` — the AP starts execution at
     * that real-mode segment with offset 0. So the trampoline must
     * be page-aligned and below 1 MiB. */
    uint32_t vector = (entry_phys >> 12) & 0xFFu;

    lapic_write(LAPIC_REG_ICR_HIGH, dest_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_DELIVERY_STARTUP |
                LAPIC_ICR_LEVEL_ASSERT |
                vector);
    lapic_ipi_wait();
}

void lapic_send_fixed(uint32_t dest_apic_id, uint8_t vector) {
    lapic_write(LAPIC_REG_ICR_HIGH, dest_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_DELIVERY_FIXED |
                LAPIC_ICR_LEVEL_ASSERT |
                vector);
    lapic_ipi_wait();
}

/* Per-CPU periodic timer. Each CPU programs its own LAPIC timer to
 * fire at `vector = LAPIC_TIMER_VECTOR` (0x40) every `initial_count`
 * LAPIC bus clocks. The handler (kernel/isr.c) calls schedule() then
 * lapic_eoi().
 *
 * Reasonable default: initial_count = 10_000_000. On QEMU that's
 * roughly 10ms (the LAPIC bus clock isn't precisely modelled). The
 * exact frequency isn't critical — frequent enough for responsive
 * preemption but not so frequent the IRQ handler dominates the CPU. */
void lapic_timer_init(uint32_t initial_count) {
    /* Set divider FIRST. Some CPUs latch the LVT entry's vector at
     * the moment LVT is written; if we wrote LVT before the divider
     * the first tick could fire with a stale period. */
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIVIDER);
    lapic_write(LAPIC_REG_LVT_TIMER,
                LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT, initial_count);
}

void lapic_timer_stop(void) {
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_MASK);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
}
