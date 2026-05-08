#ifndef ADVENTOS_TSS_H
#define ADVENTOS_TSS_H

#include "../include/types.h"

struct tss {
    uint32_t prev_tss;
    uint32_t esp0;        /* used on ring 3 -> ring 0 transitions */
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

void              tss_init(void);
void              tss_set_kernel_stack(uint32_t esp);
struct tss       *tss_get(void);
size_t            tss_size(void);

#endif
