#ifndef ADVENTOS_KPRINTF_H
#define ADVENTOS_KPRINTF_H

#include "../include/types.h"

#include <stdarg.h>

/* Output goes to both VGA text mode and the COM1 serial port. */
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list args);

#endif
