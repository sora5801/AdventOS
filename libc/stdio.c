/*
 * AdventOS libc — stdio.h implementation.
 *
 * The printf family routes to a small core that takes a "putc-like"
 * sink (function pointer + cookie). Three sinks:
 *   - sink_fd:   writes to a file descriptor via SYS_WRITE_FD
 *   - sink_str:  appends to a buffer (for sprintf)
 *   - sink_strn: same with a length cap (for snprintf)
 *
 * vprintf uses sink_fd with cookie=1 (stdout). vsprintf / vsnprintf
 * use the buffer sinks. The format-string parser is shared.
 */
#include "libc.h"

/* Internal-only sys_write wrapper. Same shape as user's, but
 * inlined here so libc doesn't depend on user/libuser. */
static int sys_write_fd(int fd, const void *buf, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_WRITE_FD), "b"(fd), "c"(buf), "d"(n)
                      : "memory");
    return ret;
}

/* ---- output sink layer ------------------------------------------- */

struct sink {
    void  (*putc)(struct sink *s, char c);
    /* For sink_fd: cookie = fd. For sink_str: cookie = char *write_pos.
     * For sink_strn: cookie = char *, cap stored alongside. */
    int     fd;
    char   *buf;
    size_t  cap;
    size_t  len;       /* characters consumed (not necessarily written) */
};

static void sink_fd_putc(struct sink *s, char c) {
    sys_write_fd(s->fd, &c, 1);
    s->len++;
}

static void sink_str_putc(struct sink *s, char c) {
    s->buf[s->len++] = c;
}

static void sink_strn_putc(struct sink *s, char c) {
    /* Always count, but only write if cap > 1 (leave 1 for NUL). */
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

/* ---- shared formatter -------------------------------------------- */

static void put_dec_signed(struct sink *s, int32_t n) {
    char buf[12]; int i = 0; int neg = 0; uint32_t u;
    if (n < 0) { neg = 1; u = (uint32_t)(-n); } else u = (uint32_t)n;
    if (u == 0) { s->putc(s, '0'); return; }
    while (u) { buf[i++] = (char)('0' + u % 10); u /= 10; }
    if (neg) s->putc(s, '-');
    while (i--) s->putc(s, buf[i]);
}

static void put_dec_unsigned(struct sink *s, uint32_t u) {
    char buf[12]; int i = 0;
    if (u == 0) { s->putc(s, '0'); return; }
    while (u) { buf[i++] = (char)('0' + u % 10); u /= 10; }
    while (i--) s->putc(s, buf[i]);
}

static void put_hex(struct sink *s, uint32_t u, int upper) {
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digits = upper ? hi : lo;
    char buf[8]; int i = 0;
    if (u == 0) { s->putc(s, '0'); return; }
    while (u) { buf[i++] = digits[u & 0xF]; u >>= 4; }
    while (i--) s->putc(s, buf[i]);
}

/* Session 48: octal. Used for Unix file modes — printf("%o", 0644) → "644". */
static void put_oct(struct sink *s, uint32_t u) {
    char buf[12]; int i = 0;
    if (u == 0) { s->putc(s, '0'); return; }
    while (u) { buf[i++] = (char)('0' + (u & 0x7)); u >>= 3; }
    while (i--) s->putc(s, buf[i]);
}

static int do_format(struct sink *s, const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') { s->putc(s, *fmt++); continue; }
        fmt++;
        switch (*fmt) {
            case 'c': s->putc(s, (char)va_arg(ap, int)); break;
            case 's': {
                const char *p = va_arg(ap, const char *);
                if (!p) p = "(null)";
                while (*p) s->putc(s, *p++);
                break;
            }
            case 'd': case 'i': put_dec_signed  (s, va_arg(ap, int32_t)); break;
            case 'u':           put_dec_unsigned(s, va_arg(ap, uint32_t)); break;
            case 'x':           put_hex         (s, va_arg(ap, uint32_t), 0); break;
            case 'X':           put_hex         (s, va_arg(ap, uint32_t), 1); break;
            case 'o':           put_oct         (s, va_arg(ap, uint32_t)); break;
            case 'p': {
                s->putc(s, '0'); s->putc(s, 'x');
                put_hex(s, (uint32_t)(uintptr_t)va_arg(ap, void *), 0);
                break;
            }
            case '%': s->putc(s, '%'); break;
            default:  s->putc(s, '%'); s->putc(s, *fmt); break;
        }
        if (*fmt) fmt++;
    }
    return (int)s->len;
}

/* ---- public entry points ----------------------------------------- */

int vprintf_(const char *fmt, va_list ap) {
    struct sink s = { sink_fd_putc, 1, 0, 0, 0 };
    return do_format(&s, fmt, ap);
}

int vsprintf_(char *buf, const char *fmt, va_list ap) {
    struct sink s = { sink_str_putc, 0, buf, 0, 0 };
    int n = do_format(&s, fmt, ap);
    buf[n] = 0;
    return n;
}

int vsnprintf_(char *buf, size_t cap, const char *fmt, va_list ap) {
    if (cap == 0) {
        struct sink s = { sink_strn_putc, 0, NULL, 0, 0 };
        return do_format(&s, fmt, ap);
    }
    struct sink s = { sink_strn_putc, 0, buf, cap, 0 };
    int total = do_format(&s, fmt, ap);
    /* NUL-terminate at min(total, cap-1). */
    size_t end = (s.len < cap) ? s.len : cap - 1;
    buf[end] = 0;
    return total;
}

void putchar_(char c) {
    sys_write_fd(1, &c, 1);
}

void puts_(const char *s) {
    int n = 0;
    while (s[n]) n++;
    sys_write_fd(1, s, n);
}
