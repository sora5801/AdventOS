#ifndef ADVENTOS_LAPIC_H
#define ADVENTOS_LAPIC_H

#include "../include/types.h"

/*
 * Local APIC (LAPIC) — per-CPU interrupt controller, present on every
 * CPU since the Pentium era. We use it for:
 *   - Identifying which CPU we're running on (LAPIC ID register)
 *   - Sending Inter-Processor Interrupts (IPIs) for AP startup and,
 *     in the future, TLB shootdowns and reschedule-now signals
 *   - Acknowledging interrupts (EOI register)
 *
 * The LAPIC's MMIO live at physical 0xFEE00000 by default (the IA32
 * APIC_BASE MSR can relocate it but nothing in our boot path does).
 * That's above the 1 MiB real-mode region, so we identity-map a
 * single 4 KiB page at that physical address into our PD before
 * touching the LAPIC.
 *
 * Each CPU sees the same VA but reads/writes go to its own local
 * APIC silicon — the MMIO is per-CPU even though the address is
 * shared. This is why we map it once and the same mapping works
 * for every core.
 */

#define LAPIC_PHYS_BASE  0xFEE00000u
#define LAPIC_VIRT_BASE  0xFEE00000u   /* identity-mapped */

/* LAPIC register offsets (in bytes from the MMIO base). All 32-bit
 * reads/writes; aligned access required. */
#define LAPIC_REG_ID         0x020   /* logical APIC ID, low 8 bits */
#define LAPIC_REG_VERSION    0x030
#define LAPIC_REG_TPR        0x080   /* task priority */
#define LAPIC_REG_EOI        0x0B0   /* end-of-interrupt: write any value */
#define LAPIC_REG_LDR        0x0D0   /* logical destination */
#define LAPIC_REG_DFR        0x0E0   /* destination format (flat=0xF, cluster=0x0) */
#define LAPIC_REG_SVR        0x0F0   /* spurious-interrupt vector */
#define LAPIC_REG_ESR        0x280   /* error status */
#define LAPIC_REG_ICR_LOW    0x300   /* interrupt command low */
#define LAPIC_REG_ICR_HIGH   0x310   /* interrupt command high (target ID) */
#define LAPIC_REG_LVT_TIMER  0x320
#define LAPIC_REG_LVT_LINT0  0x350
#define LAPIC_REG_LVT_LINT1  0x360
#define LAPIC_REG_LVT_ERROR  0x370
#define LAPIC_REG_TIMER_INIT 0x380   /* initial count for one-shot/periodic */
#define LAPIC_REG_TIMER_CUR  0x390   /* current count */
#define LAPIC_REG_TIMER_DIV  0x3E0   /* divide configuration */

/* Spurious vector register flags. Bit 8 = enable LAPIC. */
#define LAPIC_SVR_ENABLE     0x100u

/* ICR_LOW bitfields for IPIs. */
#define LAPIC_ICR_DELIVERY_FIXED   (0u << 8)
#define LAPIC_ICR_DELIVERY_INIT    (5u << 8)
#define LAPIC_ICR_DELIVERY_STARTUP (6u << 8)
#define LAPIC_ICR_LEVEL_ASSERT     (1u << 14)
#define LAPIC_ICR_TRIGGER_EDGE     (0u << 15)
#define LAPIC_ICR_TRIGGER_LEVEL    (1u << 15)
#define LAPIC_ICR_DEST_SELF        (1u << 18)
#define LAPIC_ICR_DEST_ALL_INC     (2u << 18)
#define LAPIC_ICR_DEST_ALL_EXC     (3u << 18)
#define LAPIC_ICR_PENDING          (1u << 12)

/* Map LAPIC MMIO + enable. Idempotent — the BSP calls this once at
 * smp_init time, and each AP calls it again from ap_main (the page
 * is already mapped via the shared kernel PD; lapic_enable just
 * twiddles the SVR for that core's silicon). */
void     lapic_init(void);
void     lapic_enable(void);

uint32_t lapic_id(void);                    /* this CPU's APIC ID */
void     lapic_eoi(void);

/* Send an IPI. `dest_apic_id` selects the target CPU. `vector`
 * is the IDT vector for fixed-delivery IPIs; ignored for INIT/SIPI. */
void     lapic_send_init   (uint32_t dest_apic_id);
void     lapic_send_sipi   (uint32_t dest_apic_id, uint32_t entry_phys);
void     lapic_send_fixed  (uint32_t dest_apic_id, uint8_t vector);

/* Configure THIS CPU's LAPIC timer to fire at `hz` Hz on the given
 * IDT vector, in periodic mode. Each CPU calls this on itself.
 * `count` is the number of LAPIC bus clocks per tick — we don't
 * calibrate against the PIT (a hard-coded reasonable value works
 * fine on QEMU); `divisor_cfg` selects the LAPIC's own /16 (=0x3)
 * vs /1 (=0xB) divider. */
#define LAPIC_TIMER_VECTOR   0x40
#define LAPIC_TIMER_DIVIDER  0xB    /* divide by 1: see SDM 10.5.4 */
#define LAPIC_TIMER_PERIODIC (1u << 17)
#define LAPIC_TIMER_MASK     (1u << 16)

void     lapic_timer_init  (uint32_t initial_count);
void     lapic_timer_stop  (void);

#endif
