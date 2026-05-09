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

int sys_putchar(char c) {
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

int sys_read_line(char *buf, int cap) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_READ_LINE), "b"(buf), "c"(cap)
                      : "memory");
    return ret;
}

int sys_open(const char *name) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_OPEN), "b"(name)
                      : "memory");
    return ret;
}

int sys_read(int fd, void *buf, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_READ), "b"(fd), "c"(buf), "d"(n)
                      : "memory");
    return ret;
}

int sys_write(int fd, const void *buf, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WRITE_FD), "b"(fd), "c"(buf), "d"(n)
                      : "memory");
    return ret;
}

int sys_close(int fd) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_CLOSE), "b"(fd)
                      : "memory");
    return ret;
}

int sys_socket(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SOCKET)
                      : "memory");
    return ret;
}

int sys_bind(int fd, int port) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BIND), "b"(fd), "c"(port)
                      : "memory");
    return ret;
}

int sys_listen(int fd, int backlog) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_LISTEN), "b"(fd), "c"(backlog)
                      : "memory");
    return ret;
}

int sys_accept(int fd) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_ACCEPT), "b"(fd)
                      : "memory");
    return ret;
}

int sys_fork(void) {
    int ret;
    /* The kernel synthesizes the child's stack so its first user
     * instruction is the one immediately after `int $0x80`, with
     * EAX=0. So control returns from this asm twice: in the parent
     * with EAX=child_pid, in the child with EAX=0. */
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FORK)
                      : "memory");
    return ret;
}

int sys_exec(const char *path, const char *const *argv) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_EXEC), "b"(path), "c"(argv)
                      : "memory");
    return ret;
}

int sys_wait(int *exit_code) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WAIT), "b"(exit_code)
                      : "memory");
    return ret;
}

int sys_pipe(int fds[2]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_PIPE), "b"(fds)
                      : "memory");
    return ret;
}

int sys_dup2(int oldfd, int newfd) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_DUP2), "b"(oldfd), "c"(newfd)
                      : "memory");
    return ret;
}

int sys_open_w(const char *name) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_OPEN_W), "b"(name)
                      : "memory");
    return ret;
}

/* ---------- Output --------------------------------------------------- */

/*
 * stdout-aware versions. Going through SYS_WRITE_FD with fd=1 means
 * dup2(pipe_w, 1) before exec actually causes our printf output to
 * land in the pipe instead of the console — which is the whole point
 * of session 15. The legacy sys_putchar / sys_write_str syscalls
 * still exist (the .up1/.up2 asm demos use them by number) but the
 * higher-level libuser stack no longer calls them.
 */
void putchar(char c)        { sys_write(1, &c, 1); }
void puts(const char *s)    {
    int n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

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

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
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
