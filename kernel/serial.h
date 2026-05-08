#ifndef ADVENTOS_SERIAL_H
#define ADVENTOS_SERIAL_H

#include "../include/types.h"

#define COM1_PORT 0x3F8
#define COM1_IRQ  4

void serial_init(void);
void serial_install_irq(void);   /* call after IDT/PIC are ready */
void serial_putc(char c);
void serial_write(const char *s);
int  serial_has_data(void);
char serial_getc(void);
int  serial_buf_has_data(void);
char serial_buf_pop(void);

#endif
