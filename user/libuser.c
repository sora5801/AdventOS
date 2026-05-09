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

int sys_connect(int fd, const unsigned char ip[4], int port) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_CONNECT), "b"(fd), "c"(ip), "d"(port)
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

int sys_wait_nb(int *exit_code) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WAIT_NB), "b"(exit_code)
                      : "memory");
    return ret;
}

int sys_getcpu(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETCPU)
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

int sys_kill(int pid, int sig) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_KILL), "b"(pid), "c"(sig)
                      : "memory");
    return ret;
}

uint32_t tty_set_mode(uint32_t flags) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TTY_SET_MODE), "b"(flags)
                      : "memory");
    return (uint32_t)ret;
}

uint32_t tty_get_mode(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TTY_GET_MODE)
                      : "memory");
    return (uint32_t)ret;
}

int tty_inject(const char *bytes, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TTY_INJECT), "b"(bytes), "c"(n)
                      : "memory");
    return ret;
}

int sys_fs_write(const char *name, const void *data, uint32_t n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FS_WRITE), "b"(name), "c"(data), "d"(n)
                      : "memory");
    return ret;
}

int setpgid(int pid, int pgid) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SETPGID), "b"(pid), "c"(pgid)
                      : "memory");
    return ret;
}

int getpgid(int pid) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETPGID), "b"(pid)
                      : "memory");
    return ret;
}

int setsid(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SETSID)
                      : "memory");
    return ret;
}

int getsid(int pid) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETSID), "b"(pid)
                      : "memory");
    return ret;
}

int killpg(int pgid, int sig) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_KILLPG), "b"(pgid), "c"(sig)
                      : "memory");
    return ret;
}

int tcsetpgrp(int fd, int pgid) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TCSETPGRP), "b"(fd), "c"(pgid)
                      : "memory");
    return ret;
}

int tcgetpgrp(int fd) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TCGETPGRP), "b"(fd)
                      : "memory");
    return ret;
}

int sys_dns_resolve(const char *name, unsigned char ip[4]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_DNS_RESOLVE), "b"(name), "c"(ip)
                      : "memory");
    return ret;
}

uint32_t sys_fs_free_sectors(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FS_FREE_SECTORS)
                      : "memory");
    return (uint32_t)ret;
}

void *sys_mmap(int fd, uint32_t offset, uint32_t length) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_MMAP), "b"(fd), "c"(offset), "d"(length)
                      : "memory");
    return (void *)(uint32_t)ret;
}

int sys_munmap(void *addr, uint32_t length) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_MUNMAP), "b"(addr), "c"(length)
                      : "memory");
    return ret;
}

int sys_mkdir(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_MKDIR), "b"(path)
                      : "memory");
    return ret;
}

int sys_chdir(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_CHDIR), "b"(path)
                      : "memory");
    return ret;
}

int sys_getcwd(char *buf, int cap) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETCWD), "b"(buf), "c"(cap)
                      : "memory");
    return ret;
}

int sys_readdir(const char *dir_path, int *iter, char *name_buf) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_READDIR), "b"(dir_path), "c"(iter), "d"(name_buf)
                      : "memory");
    return ret;
}

uint32_t sys_bcache_sync(void) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BCACHE_SYNC)
                      : "memory");
    return ret;
}

int sys_bcache_stats(uint32_t out[5]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BCACHE_STATS), "b"(out)
                      : "memory");
    return ret;
}

/*
 * Sigreturn trampoline. The kernel pushes (low to high on user stack):
 *   [trampoline_addr]   ← handler's "return address"
 *   [sig_num]           ← handler's arg
 *   [sigcontext: 64B]
 * before iretting into the user handler. When the handler ret's, ESP
 * lands at sig_num (handler popped trampoline_addr); we add 4 to skip
 * past sig_num so SIGRETURN sees ESP pointing exactly at sigcontext.
 */
/* Defined in asm so we control the exact instruction sequence. The
 * C-visible name is `sigreturn_tramp`; mingw32 prepends an underscore
 * to map C symbols to asm symbols, so the asm label is `_sigreturn_tramp`.
 */
__asm__ (
    ".global _sigreturn_tramp        \n"
    "_sigreturn_tramp:               \n"
    "    add    $4, %esp             \n"
    "    mov    $26, %eax            \n"   /* SYS_SIGRETURN */
    "    int    $0x80                \n"
    /* Should never return — sigreturn restores the saved frame and
     * irets to the pre-signal EIP/ESP. Loop just in case something
     * goes wrong. */
    "1:  hlt                         \n"
    "    jmp 1b                      \n"
);

extern void sigreturn_tramp(void);   /* implemented above in asm */

sighandler_t sigaction(int sig, sighandler_t handler) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SIGACTION), "b"(sig),
                        "c"(handler), "d"(sigreturn_tramp)
                      : "memory");
    return (sighandler_t)(uint32_t)ret;
}

sighandler_t signal(int sig, sighandler_t handler) {
    /* POSIX `signal()` is just a thin wrapper. Kept separate so user
     * code can use either name. */
    return sigaction(sig, handler);
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

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *aa = a;
    const unsigned char *bb = b;
    while (n--) {
        if (*aa != *bb) return (int)*aa - (int)*bb;
        aa++; bb++;
    }
    return 0;
}

/* Decimal-only atoi. Skips leading whitespace; honors a single +/- sign;
 * stops at the first non-digit. No overflow detection — callers in the
 * coreutils sweep pass small counts (line counts, signal numbers, pids)
 * that comfortably fit in int. */
int atoi(const char *s) {
    int v = 0, sign = 1;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if      (*s == '-') { sign = -1; s++; }
    else if (*s == '+') {             s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

const char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return s;
        s++;
    }
    return (ch == 0) ? s : 0;
}

/* ---------- Heap: sys_brk + malloc / free --------------------------- */

/*
 * Port of the kernel's kmalloc free-list allocator into ring 3.
 * Headered blocks threaded in physical-address order; first-fit,
 * split-on-alloc, neighbor-coalesce on free.
 *
 * The heap grows on demand: when malloc can't satisfy a request from
 * the existing free list, it issues SYS_BRK to map new pages between
 * the current break and a target break, then drops a fresh free
 * block on top of the new pages and retries.
 *
 * Allocator state is just `g_brk`. The list head is implicit:
 * heap-empty iff g_brk == HEAP_START_VA, else the head is the
 * mblock at HEAP_START_VA. This avoids needing a separate `g_head`
 * pointer in .data — explained in detail in the deep dive.
 */

#define HEAP_START_VA   0x40200000u
#define M_ALIGN         16
#define M_HDR_SIZE      sizeof(struct mblock)
#define M_MIN_PAYLOAD   16u
#define M_MAGIC         0xCAFEu

struct mblock {
    uint32_t        size;
    uint16_t        free;
    uint16_t        magic;
    struct mblock  *prev;
    struct mblock  *next;
};

/* IMPORTANT: explicit non-zero initializer. user.ld discards .bss,
 * which means a zero-initialized file-scope static would be linked
 * at address 0. Setting g_brk = HEAP_START_VA both gives it the
 * right initial value AND forces it into .data so the loader sets
 * it correctly. */
static uint32_t g_brk = HEAP_START_VA;

int sys_brk(int new_brk) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BRK), "b"(new_brk)
                      : "memory");
    return ret;
}

static struct mblock *heap_head(void) {
    return (g_brk == HEAP_START_VA) ? (struct mblock *)0
                                    : (struct mblock *)HEAP_START_VA;
}

static struct mblock *heap_tail(void) {
    struct mblock *b = heap_head();
    if (!b) return (struct mblock *)0;
    while (b->next) b = b->next;
    return b;
}

/* Ask the kernel for at least `at_least` more bytes of heap (rounded
 * up to a page), then drop a fresh free block on the new pages. If
 * the previous tail block was free, coalesce. Returns 0 on success,
 * -1 on out-of-memory. */
static int grow_heap(uint32_t at_least) {
    uint32_t need = (at_least + 4095u) & ~4095u;
    uint32_t want = g_brk + need;
    int got = sys_brk((int)want);
    if (got != (int)want) return -1;

    uint32_t old = g_brk;
    g_brk = (uint32_t)got;

    /* New free block at [old, g_brk). */
    struct mblock *nb = (struct mblock *)(uint32_t)old;
    nb->size  = (uint32_t)(g_brk - old - M_HDR_SIZE);
    nb->free  = 1;
    nb->magic = M_MAGIC;
    nb->prev  = (struct mblock *)0;
    nb->next  = (struct mblock *)0;

    if (old == HEAP_START_VA) {
        /* First growth — nb is the head AND the tail. Done. */
        return 0;
    }

    /* Link after current tail. */
    struct mblock *tail = heap_tail();
    if (!tail) return -1;
    tail->next = nb;
    nb->prev   = tail;

    /* If the old tail was free, coalesce nb into it. */
    if (tail->free) {
        tail->size += M_HDR_SIZE + nb->size;
        tail->next  = (struct mblock *)0;
    }
    return 0;
}

void *malloc(size_t size) {
    if (size == 0) return (void *)0;
    size_t want = (size + M_ALIGN - 1) & ~(size_t)(M_ALIGN - 1);

    for (;;) {
        for (struct mblock *b = heap_head(); b; b = b->next) {
            if (b->magic != M_MAGIC) return (void *)0;     /* corruption */
            if (!b->free || b->size < want) continue;

            /* Found a fit. Split if leftover would be a usable block. */
            size_t leftover = b->size - want;
            if (leftover >= M_HDR_SIZE + M_MIN_PAYLOAD) {
                struct mblock *n =
                    (struct mblock *)((uint32_t)b + M_HDR_SIZE + want);
                n->size  = (uint32_t)(leftover - M_HDR_SIZE);
                n->free  = 1;
                n->magic = M_MAGIC;
                n->prev  = b;
                n->next  = b->next;
                if (b->next) b->next->prev = n;
                b->next  = n;
                b->size  = (uint32_t)want;
            }
            b->free = 0;
            return (void *)((uint32_t)b + M_HDR_SIZE);
        }

        /* No fit — grow and retry. Bail if the kernel says no. */
        if (grow_heap((uint32_t)(want + M_HDR_SIZE)) != 0) return (void *)0;
    }
}

void free(void *p) {
    if (!p) return;
    struct mblock *b = (struct mblock *)((uint32_t)p - M_HDR_SIZE);
    if (b->magic != M_MAGIC || b->free) return;       /* double-free / corrupt */
    b->free = 1;

    /* Coalesce with the next block if free. */
    if (b->next && b->next->free) {
        struct mblock *n = b->next;
        b->size += M_HDR_SIZE + n->size;
        b->next  = n->next;
        if (n->next) n->next->prev = b;
        n->magic = 0;
    }

    /* Coalesce with the previous block if free. */
    if (b->prev && b->prev->free) {
        struct mblock *pp = b->prev;
        pp->size += M_HDR_SIZE + b->size;
        pp->next  = b->next;
        if (b->next) b->next->prev = pp;
        b->magic = 0;
    }
}

uint32_t malloc_brk(void) { return g_brk; }

uint32_t malloc_total(void) {
    return g_brk - HEAP_START_VA;
}

uint32_t malloc_used(void) {
    uint32_t total = 0;
    for (struct mblock *b = heap_head(); b; b = b->next) {
        if (!b->free) total += b->size + M_HDR_SIZE;
    }
    return total;
}

uint32_t malloc_free_bytes(void) {
    uint32_t total = 0;
    for (struct mblock *b = heap_head(); b; b = b->next) {
        if (b->free) total += b->size;
    }
    return total;
}
