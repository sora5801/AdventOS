#ifndef ADVENTOS_IDT_H
#define ADVENTOS_IDT_H

#include "../include/types.h"

#define IDT_NUM_ENTRIES 256

void idt_init(void);
void idt_set_gate(int n, uint32_t handler, uint16_t selector, uint8_t flags);

#endif
