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

int sys_fbinfo(unsigned int out[4]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FBINFO), "b"(out)
                      : "memory");
    return ret;
}

int sys_smp_stats(unsigned int out[8]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SMP_STATS), "b"(out)
                      : "memory");
    return ret;
}

int sys_mouse_state(int out[4]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_MOUSE_STATE), "b"(out)
                      : "memory");
    return ret;
}

void *sys_fb_mmap(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FB_MMAP)
                      : "memory");
    return (void *)(unsigned long)ret;
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

/* ---------- Dynamic libc trampolines -------------------------------
 *
 * Session 35 moved the C library implementations out of libuser into
 * libc.bin, which the kernel's dyld layer maps into every new user
 * process at LIBC_BASE = 0x70000000. The first 16 bytes are a
 * magic/version header; the next 256 bytes are a fixed-order array
 * of function pointers (the EXPORT TABLE).
 *
 * Each function below is a thin trampoline that loads the right
 * pointer out of the table and calls into libc. The trampolines are
 * REAL functions (not macros) so taking their address still works
 * (e.g., `int (*p)(const char *) = strcmp` continues to compile).
 *
 * For variadic printf-family functions, we use va_list versions in
 * libc (vprintf/vsprintf/vsnprintf) and wrap them with va_start/end
 * here. That avoids needing GCC's __builtin_va_arg_pack to forward
 * the original variadic arglist through the indirect call. */

#define LIBC_BASE          0x70000000u
#define LIBC_TABLE_OFF     0x10u
#define LIBC_TABLE         ((void * const *)(LIBC_BASE + LIBC_TABLE_OFF))

/* Indices MUST agree with libc/libc.h's enum. */
#define LIBC_FN_LIBC_INFO    0
#define LIBC_FN_STRLEN       1
#define LIBC_FN_STRCMP       2
#define LIBC_FN_STRNCMP      3
#define LIBC_FN_STRCPY       4
#define LIBC_FN_STRNCPY      5
#define LIBC_FN_STRCAT       6
#define LIBC_FN_STRCHR       7
#define LIBC_FN_STRRCHR      8
#define LIBC_FN_STRSTR       9
#define LIBC_FN_MEMSET      10
#define LIBC_FN_MEMCPY      11
#define LIBC_FN_MEMMOVE     12
#define LIBC_FN_MEMCMP      13
#define LIBC_FN_MEMCHR      14
#define LIBC_FN_ATOI        20
#define LIBC_FN_ATOL        21
#define LIBC_FN_STRTOL      22
#define LIBC_FN_ABS         23
#define LIBC_FN_MALLOC      24
#define LIBC_FN_FREE        25
#define LIBC_FN_CALLOC      26
#define LIBC_FN_REALLOC     27
#define LIBC_FN_PUTCHAR     40
#define LIBC_FN_PUTS        41
#define LIBC_FN_VPRINTF     42
#define LIBC_FN_VSPRINTF    43
#define LIBC_FN_VSNPRINTF   44
#define LIBC_FN_MALLOC_BRK   50
#define LIBC_FN_MALLOC_USED  51
#define LIBC_FN_MALLOC_FREE_ 52
#define LIBC_FN_MALLOC_TOTAL 53

/* String — direct pass-through. */
size_t strlen(const char *s) {
    return ((size_t (*)(const char *))LIBC_TABLE[LIBC_FN_STRLEN])(s);
}
int strcmp(const char *a, const char *b) {
    return ((int (*)(const char *, const char *))LIBC_TABLE[LIBC_FN_STRCMP])(a, b);
}
int strncmp(const char *a, const char *b, size_t n) {
    return ((int (*)(const char *, const char *, size_t))
            LIBC_TABLE[LIBC_FN_STRNCMP])(a, b, n);
}
const char *strchr(const char *s, int c) {
    return ((const char *(*)(const char *, int))LIBC_TABLE[LIBC_FN_STRCHR])(s, c);
}

/* Memory. */
void *memset(void *p, int c, size_t n) {
    return ((void *(*)(void *, int, size_t))LIBC_TABLE[LIBC_FN_MEMSET])(p, c, n);
}
void *memcpy(void *d, const void *s, size_t n) {
    return ((void *(*)(void *, const void *, size_t))
            LIBC_TABLE[LIBC_FN_MEMCPY])(d, s, n);
}
int memcmp(const void *a, const void *b, size_t n) {
    return ((int (*)(const void *, const void *, size_t))
            LIBC_TABLE[LIBC_FN_MEMCMP])(a, b, n);
}

/* stdlib. */
int atoi(const char *s) {
    return ((int (*)(const char *))LIBC_TABLE[LIBC_FN_ATOI])(s);
}

/* Heap — trampoline through libc's malloc/free.  Each user process
 * has its own libc.bin .data so the heap state stays per-process. */
void *malloc(size_t size) {
    return ((void *(*)(size_t))LIBC_TABLE[LIBC_FN_MALLOC])(size);
}
void free(void *p) {
    ((void (*)(void *))LIBC_TABLE[LIBC_FN_FREE])(p);
}
uint32_t malloc_brk(void) {
    return ((uint32_t (*)(void))LIBC_TABLE[LIBC_FN_MALLOC_BRK])();
}
uint32_t malloc_total(void) {
    return ((uint32_t (*)(void))LIBC_TABLE[LIBC_FN_MALLOC_TOTAL])();
}
uint32_t malloc_used(void) {
    return ((uint32_t (*)(void))LIBC_TABLE[LIBC_FN_MALLOC_USED])();
}
uint32_t malloc_free_bytes(void) {
    return ((uint32_t (*)(void))LIBC_TABLE[LIBC_FN_MALLOC_FREE_])();
}

/* Stdio. putchar/puts forward straight into libc's putchar_/puts_. */
void putchar(char c) {
    ((void (*)(char))LIBC_TABLE[LIBC_FN_PUTCHAR])(c);
}
void puts(const char *s) {
    ((void (*)(const char *))LIBC_TABLE[LIBC_FN_PUTS])(s);
}

/* printf: shim wraps va_list and dispatches into libc's vprintf_. */
void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ((int (*)(const char *, va_list))LIBC_TABLE[LIBC_FN_VPRINTF])(fmt, args);
    va_end(args);
}

/* Non-zero-initialized marker: forces user.ld's .data section to be
 * emitted with CONTENTS (not as a NOBITS section the linker would
 * collapse). Without this, an init.c-style program with an
 * uninitialized `static char buf[2048]` would have buf living in a
 * .data region that objcopy strips from the flat binary, leaving
 * the kernel ELF loader to map only the .text+.rdata pages — and
 * the very first sys_read into buf would page-fault on the unmapped
 * tail half. The legacy libuser had an initialized global (g_brk =
 * HEAP_START_VA) that accidentally papered over this; we now keep
 * a single explicit marker.
 *
 * `used` keeps the symbol from being garbage-collected even though
 * nothing references it. */
__attribute__((used))
static uint32_t libuser_data_marker = 0xADBEEF35u;

/* sys_brk: only syscall left in libuser that's directly tied to the
 * heap. malloc itself lives in libc.bin (so the implementation is
 * shared across processes); each process's libc.data has its own
 * g_brk pointer. We expose sys_brk here for any user program that
 * wants to manage its own break independently of malloc — and for
 * the [t25] selftest's diagnostics. */
int sys_brk(int new_brk) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BRK), "b"(new_brk)
                      : "memory");
    return ret;
}
