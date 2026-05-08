#ifndef ADVENTOS_PIC_H
#define ADVENTOS_PIC_H

#include "../include/types.h"

#define PIC1_OFFSET 0x20    /* IRQ 0..7  -> vectors 0x20..0x27 */
#define PIC2_OFFSET 0x28    /* IRQ 8..15 -> vectors 0x28..0x2F */

void pic_remap(uint8_t offset1, uint8_t offset2);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#endif
