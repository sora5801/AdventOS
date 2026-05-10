#include "kprintf.h"
#include "vga.h"
#include "serial.h"
#include "fbcon.h"
#include "string.h"
#include "spinlock.h"

/* Serializer for the three console sinks. Without this, two CPUs
 * calling kprintf concurrently produce byte-by-byte interleaved
 * output ("init: stinit: exec failed" instead of "init: started"
 * + "init: exec failed"). The lock is held only across the single-
 * char write to all three sinks; the rest of kvprintf's format
 * parsing runs without it (each char is independently atomic on
 * the consumer side, and reordering of independent prints is
 * tolerable — what we prevent is char-stream tearing). */
static spinlock_t g_kputc_lock = SPINLOCK_INIT;

void kputc(char c) {
    /* Three sinks in parallel:
     *  - serial: always on; needed for the host side to see early-boot
     *    panics before VGA is reachable, and for the test harness
     *  - VGA text: works from boot, ignored after VBE switch (the
     *    framebuffer overlays the text-mode 0xB8000 region but a stray
     *    text-mode write costs nothing)
     *  - fbcon: silent until fbcon_init() runs, then paints into the
     *    VBE linear FB
     * Cost in the disabled state is one branch each. */
    spin_lock(&g_kputc_lock);
    vga_putc(c);
    serial_putc(c);
    fbcon_putc(c);
    spin_unlock(&g_kputc_lock);
}

/* Higher-level lock so a multi-line kputs / kvprintf appears
 * atomically rather than interleaving char-by-char with another
 * CPU's. The per-char g_kputc_lock above protects only single-byte
 * sink writes; this one keeps related characters together. */
static spinlock_t g_kvprintf_lock = SPINLOCK_INIT;

void kputs(const char *s) {
    spin_lock(&g_kvprintf_lock);
    while (*s) kputc(*s++);
    spin_unlock(&g_kvprintf_lock);
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
    spin_lock(&g_kvprintf_lock);
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
    spin_unlock(&g_kvprintf_lock);
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}
