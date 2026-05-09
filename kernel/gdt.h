#ifndef ADVENTOS_GDT_H
#define ADVENTOS_GDT_H

#include "../include/types.h"

struct tss;     /* fwd */

void     gdt_init(void);

/* Reload GDTR + segment registers for an AP that just came up
 * through the trampoline. The trampoline's mini-GDT was a temporary;
 * APs have to load the kernel's master GDT before doing anything
 * that depends on full segment behaviour (TSS load, interrupts,
 * iret to ring 3, etc.). */
void     gdt_load_for_ap(void);

/* Reload IDTR for an AP. The IDT itself is shared; this just sets
 * the AP's idtr register. */
void     idt_load_for_ap(void);

/* Append a TSS descriptor to the GDT and return its selector
 * (multiple of 8). Used by smp_init to make a per-CPU TSS for
 * each AP. Returns 0 on out-of-slots. */
uint16_t gdt_add_tss(struct tss *tss);

#endif
