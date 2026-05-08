#include "pic.h"
#include "../include/io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

void pic_remap(uint8_t offset1, uint8_t offset2) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    outb(PIC1_DATA, offset1);              io_wait();  /* ICW2: master vector base */
    outb(PIC2_DATA, offset2);              io_wait();  /* ICW2: slave vector base  */

    outb(PIC1_DATA, 0x04);                 io_wait();  /* ICW3: tell master slave is at IRQ2 */
    outb(PIC2_DATA, 0x02);                 io_wait();  /* ICW3: tell slave its cascade id    */

    outb(PIC1_DATA, ICW4_8086);            io_wait();
    outb(PIC2_DATA, ICW4_8086);            io_wait();

    /* Restore previous masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(irq < 8 ? irq : irq - 8);
    uint8_t  m    = inb(port);
    outb(port, (uint8_t)(m | (1u << bit)));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(irq < 8 ? irq : irq - 8);
    uint8_t  m    = inb(port);
    outb(port, (uint8_t)(m & ~(1u << bit)));
}
