#include "kprintf.h"
#include "vga.h"
#include "serial.h"
#include "string.h"

void kputc(char c) {
    vga_putc(c);
    serial_putc(c);
}

void kputs(const char *s) {
    while (*s) kputc(*s++);
}

static void put_dec(int32_t n) {
    char buf[16];
    int  i = 0;
    int  neg = 0;
    uint32_t u;

    if (n < 0) { neg = 1; u = (uint32_t)(-n); } else u = (uint32_t)n;
    if (u == 0) { kputc('0'); return; }
    while (u) { buf[i++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) kputc('-');
    while (i--) kputc(buf[i]);
}

static void put_udec(uint32_t u) {
    char buf[16];
    int  i = 0;
    if (u == 0) { kputc('0'); return; }
    while (u) { buf[i++] = (char)('0' + (u % 10)); u /= 10; }
    while (i--) kputc(buf[i]);
}

static void put_hex(uint32_t u, int width, int upper) {
    char buf[16];
    int  i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (u == 0) buf[i++] = '0';
    while (u) { buf[i++] = digits[u & 0xF]; u >>= 4; }
    while (i < width) buf[i++] = '0';
    while (i--) kputc(buf[i]);
}

void kvprintf(const char *fmt, va_list args) {
    while (*fmt) {
        if (*fmt != '%') { kputc(*fmt++); continue; }
        fmt++;

        int width = 0;
        int zero_pad = 0;
        int left_align = 0;
        if (*fmt == '-') { left_align = 1; fmt++; }
        if (*fmt == '0') { zero_pad = 1;   fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        (void)zero_pad;

        switch (*fmt) {
            case 'c': {
                int c = va_arg(args, int);
                kputc((char)c);
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                int len = 0;
                for (const char *p = s; *p; p++) len++;
                int pad = (width > len) ? (width - len) : 0;
                if (!left_align) while (pad-- > 0) kputc(' ');
                for (const char *p = s; *p; p++) kputc(*p);
                if ( left_align) while (pad-- > 0) kputc(' ');
                break;
            }
            case 'd': case 'i': {
                int n = va_arg(args, int);
                put_dec(n);
                break;
            }
            case 'u': {
                unsigned u = va_arg(args, unsigned);
                put_udec(u);
                break;
            }
            case 'x': {
                unsigned u = va_arg(args, unsigned);
                put_hex(u, width, 0);
                break;
            }
            case 'X': {
                unsigned u = va_arg(args, unsigned);
                put_hex(u, width, 1);
                break;
            }
            case 'p': {
                void *p = va_arg(args, void *);
                kputc('0'); kputc('x');
                put_hex((uint32_t)(uintptr_t)p, 8, 0);
                break;
            }
            case '%':
                kputc('%');
                break;
            default:
                kputc('%');
                kputc(*fmt);
                break;
        }
        if (*fmt) fmt++;
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}
