#ifndef ADVENTOS_SERIAL_H
#define ADVENTOS_SERIAL_H

#include "../include/types.h"

#define COM1_PORT 0x3F8
#define COM1_IRQ  4

void serial_init(void);
void serial_install_irq(void);   /* call after IDT/PIC are ready */
void serial_putc(char c);
void serial_write(const char *s);

/* Polled-mode raw access — only used by very early boot code before
 * the IRQ is installed, and by the SYS_SERIAL_INJECT selftest path.
 * Once `serial_install_irq` has wired IRQ 4, real input flows through
 * `serial_inject_bytes` straight into the kbd ring; nothing else
 * polls. */
int  serial_has_data(void);
char serial_getc(void);

/* Session 67: run a byte sequence through the same translation +
 * `keyboard_inject` pipeline the IRQ handler uses. Exposed so the
 * SYS_SERIAL_INJECT syscall and [t49] selftest can drive it without
 * the byte actually crossing the UART silicon. */
void serial_inject_bytes(const char *bytes, int n);

/* Session 68 fallback: drain the UART RBR via polling. Called from
 * the PIT tick handler because IRQ 4 doesn't fire under our QEMU
 * `-display none -serial stdio` config. Cheap when there's no data
 * (one inb of LSR); cheaper than blocking on input we'll never see. */
void serial_poll_once(void);

#endif
