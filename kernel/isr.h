#ifndef ADVENTOS_ISR_H
#define ADVENTOS_ISR_H

#include "../include/types.h"

struct registers {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_orig, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
};

typedef void (*irq_handler_fn)(struct registers *r);

void isr_register_irq(int irq, irq_handler_fn handler);

#endif
