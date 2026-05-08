#include "libuser.h"

/*
 * mingw32 GCC auto-emits `call ___main` at the top of any function
 * literally named `main`. It's a libgcc hook for C++ static
 * constructor invocation, etc. We're freestanding with no such
 * machinery — provide an empty stub. (The C source name `__main`
 * gets one mingw underscore added at compile time, producing the
 * `___main` symbol the linker is looking for.)
 */
void __main(void) {}

/* ---------- Syscall wrappers --------------------------------------- */

int sys_write(char c) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WRITE), "b"((int)(unsigned char)c)
                      : "memory");
    return ret;
}

int sys_getpid(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETPID)
                      : "memory");
    return ret;
}

void sys_exit(int code) {
    __asm__ volatile ("int $0x80"
                      :
                      : "a"(SYS_EXIT), "b"(code)
                      : "memory");
    /* unreachable — but a return path satisfies the compiler */
    for (;;) __asm__ volatile ("hlt");
}

void sys_yield(void) {
    __asm__ volatile ("int $0x80"
                      :
                      : "a"(SYS_YIELD)
                      : "memory");
}

int sys_write_str(const char *s) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WRITE_STR), "b"(s)
                      : "memory");
    return ret;
}

void sys_sleep_ms(uint32_t ms) {
    __asm__ volatile ("int $0x80"
                      :
                      : "a"(SYS_SLEEP_MS), "b"(ms)
                      : "memory");
}

uint32_t sys_time(void) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TIME)
                      : "memory");
    return ret;
}

/* ---------- Output --------------------------------------------------- */

void putchar(char c) { sys_write(c); }
void puts(const char *s) { sys_write_str(s); }

static void put_dec_signed(int32_t n) {
    char buf[12];
    int  i = 0;
    int  neg = 0;
    uint32_t u;
    if (n < 0) { neg = 1; u = (uint32_t)(-n); } else u = (uint32_t)n;
    if (u == 0) { putchar('0'); return; }
    while (u) { buf[i++] = (char)('0' + u % 10); u /= 10; }
    if (neg) putchar('-');
    while (i--) putchar(buf[i]);
}

static void put_dec_unsigned(uint32_t u) {
    char buf[12];
    int  i = 0;
    if (u == 0) { putchar('0'); return; }
    while (u) { buf[i++] = (char)('0' + u % 10); u /= 10; }
    while (i--) putchar(buf[i]);
}

static void put_hex(uint32_t u) {
    static const char digits[] = "0123456789abcdef";
    char buf[8];
    int  i = 0;
    if (u == 0) { putchar('0'); return; }
    while (u) { buf[i++] = digits[u & 0xF]; u >>= 4; }
    while (i--) putchar(buf[i]);
}

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    while (*fmt) {
        if (*fmt != '%') { putchar(*fmt++); continue; }
        fmt++;
        switch (*fmt) {
            case 'c': putchar((char)va_arg(args, int)); break;
            case 's': {
                const char *s = va_arg(args, const char *);
                while (*s) putchar(*s++);
                break;
            }
            case 'd': case 'i': put_dec_signed  (va_arg(args, int32_t));  break;
            case 'u':           put_dec_unsigned(va_arg(args, uint32_t)); break;
            case 'x':           put_hex         (va_arg(args, uint32_t)); break;
            case '%':           putchar('%'); break;
            default:            putchar('%'); putchar(*fmt); break;
        }
        if (*fmt) fmt++;
    }
    va_end(args);
}

/* ---------- Tiny C library bits ------------------------------------- */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void *memset(void *p, int c, size_t n) {
    unsigned char *d = p;
    while (n--) *d++ = (unsigned char)c;
    return p;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char       *dd = d;
    const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
