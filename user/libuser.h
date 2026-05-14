#ifndef LIBUSER_H
#define LIBUSER_H

/*
 * AdventOS user-space runtime.
 *
 * Linked into every user program. Provides:
 *   - Syscall wrappers (sys_*)
 *   - putchar / puts / printf
 *   - Minimal mem* / str* utilities
 *
 * The user program supplies `main`. The provided _start stub
 * (user/start.S) calls main() and forwards its return value to
 * sys_exit, so user code looks like:
 *
 *     #include "libuser.h"
 *     int main(void) {
 *         printf("hi from pid %d\n", sys_getpid());
 *         return 0;
 *     }
 */

#include <stdarg.h>

typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef unsigned short uint16_t;
typedef unsigned int   size_t;

/* Must agree with kernel/syscall.h numbers */
#define SYS_WRITE      1
#define SYS_GETPID     2
#define SYS_EXIT       3
#define SYS_YIELD      4
#define SYS_WRITE_STR  5
#define SYS_SLEEP_MS   6
#define SYS_TIME       7
#define SYS_READ_LINE  8
/* SYS_KCMD = 9 retired in session 14 (real fork/exec/wait shell) */
#define SYS_OPEN       10
#define SYS_READ       11
#define SYS_WRITE_FD   12
#define SYS_CLOSE      13
#define SYS_SOCKET     14
#define SYS_BIND       15
#define SYS_LISTEN     16
#define SYS_ACCEPT     17
#define SYS_FORK       18
#define SYS_EXEC       19
#define SYS_WAIT       20
#define SYS_PIPE       21
#define SYS_DUP2       22
#define SYS_OPEN_W     23
#define SYS_KILL       24
#define SYS_SIGACTION  25
#define SYS_SIGRETURN  26
#define SYS_BRK        27
#define SYS_TTY_SET_MODE 28
#define SYS_TTY_GET_MODE 29
#define SYS_TTY_INJECT   30
#define SYS_FS_WRITE     31
#define SYS_SETPGID      32
#define SYS_GETPGID      33
#define SYS_SETSID       34
#define SYS_GETSID       35
#define SYS_KILLPG       36
#define SYS_TCSETPGRP    37
#define SYS_TCGETPGRP    38
#define SYS_DNS_RESOLVE  39
#define SYS_FS_FREE_SECTORS 40
#define SYS_MMAP         41
#define SYS_MUNMAP       42
#define SYS_MKDIR        43
#define SYS_CHDIR        44
#define SYS_GETCWD       45
#define SYS_READDIR      46
#define SYS_BCACHE_SYNC  47
#define SYS_BCACHE_STATS 48
#define SYS_CONNECT      49
#define SYS_WAIT_NB      50
#define SYS_GETCPU       51
#define SYS_FBINFO       52
#define SYS_SMP_STATS    53
/* 54 (SYS_MOUSE_STATE) and 55 (SYS_FB_MMAP) retired — see kernel/syscall.h.
 * Slots stay reserved for ABI stability; the dispatcher returns 0. */
#define SYS_AUDIO_PLAY   56
#define SYS_BLOCK_INFO   57
#define SYS_BLOCK_READ   58
#define SYS_BLOCK_WRITE  59
#define SYS_TTY_CURSOR    60
#define SYS_TTY_CLEAR     61
#define SYS_TTY_CLEAR_EOL 62
#define SYS_GETUID        63
#define SYS_GETGID        64
#define SYS_SETUID        65
#define SYS_SETGID        66
#define SYS_FS_OWNER      67
#define SYS_FS_MODE       68
#define SYS_CHMOD         69
#define SYS_CHOWN         70
#define SYS_OPENPTY       71

/* Session 57 — debugger plumbing (mirror of kernel/syscall.h). The
 * SYS_FB_TAKEOVER and SYS_MOUSE_INJECT slots were retired with the
 * WM and mouse driver but stay allocated for ABI stability. */
#define SYS_FB_TAKEOVER   72
#define SYS_KBD_POLL      73
#define SYS_MOUSE_INJECT  74
#define SYS_PTRACE        75
#define SYS_NTP_SYNC      76
#define SYS_NTP_TEST_RESPONDER 77
#define SYS_DNS_CACHE_STATS    78
#define SYS_DHCP_INFO     79
#define SYS_FD_NB         80
/* Session 67 — serial keyboard input testing hook. */
#define SYS_SERIAL_INJECT 81

/* Session 60 — DHCP introspection (mirror of kernel/syscall.h).
 * Note: libuser.h doesn't pull in uint8_t (only uint16/32 + size_t to
 * keep the user ABI tight), so we use unsigned char for byte fields. */
struct sys_dhcp_info {
    unsigned char ip[4];
    unsigned char netmask[4];
    unsigned char gateway[4];
    unsigned char dns_server[4];
    uint32_t      lease_seconds;
    uint32_t      acquired_epoch;
    uint32_t      t1_renew_at;
    int           have_lease;
};

/* ptrace op set — mirror of kernel/syscall.h. */
#define PTRACE_TRACEME    0
#define PTRACE_ATTACH     1
#define PTRACE_DETACH     2
#define PTRACE_PEEKDATA   3
#define PTRACE_POKEDATA   4
#define PTRACE_GETREGS    5
#define PTRACE_SETREGS    6
#define PTRACE_CONT       7
#define PTRACE_STEP       8
#define PTRACE_WAIT       9

struct ptrace_regs {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp;
    uint32_t eip, esp;
    uint32_t eflags;
};

struct ptrace_args {
    uint32_t       addr;
    uint32_t       size;
    void          *buf;
    struct ptrace_regs *regs;
};

/* Block-device ABI — must match kernel/syscall.h exactly. */
struct sys_block_info {
    unsigned int block_size;
    unsigned int n_blocks;
    char         name[16];
};
struct sys_block_args {
    unsigned int dev_idx;
    unsigned int lba;
    unsigned int n_blocks;
    void        *buf;
};

/* TTY mode flags — must agree with kernel/tty.h. */
#define TTY_ICANON   0x01
#define TTY_ECHO     0x02
#define TTY_DEFAULT  (TTY_ICANON | TTY_ECHO)
#define TTY_RAW      0u

/* Signal numbers — must match kernel/signal.h exactly. */
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGABRT     6
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGCHLD    17
#define SIGCONT    18
#define SIGSTOP    19
#define SIGTSTP    20

typedef void (*sighandler_t)(int);

#define SIG_DFL    ((sighandler_t)0)
#define SIG_IGN    ((sighandler_t)1)
#define SIG_ERR    ((sighandler_t)-1)

/* Single-char console write. The fd-aware variant (`sys_write` below)
 * is what new programs should call; this wraps the legacy SYS_WRITE
 * syscall and is kept so libuser putchar() and the in-tree
 * .up1/.up2 asm programs keep working. */
int      sys_putchar(char c);
int      sys_getpid(void);
void     sys_exit(int code) __attribute__((noreturn));
void     sys_yield(void);
int      sys_write_str(const char *s);
void     sys_sleep_ms(uint32_t ms);
uint32_t sys_time(void);
int      sys_read_line(char *buf, int cap);

/* Unix-flavored fd operations. fd 0/1/2 are pre-wired in the kernel
 * to stdin/stdout/stderr; sys_open returns a fresh descriptor for
 * filesystem files. */
int      sys_open (const char *name);
int      sys_read (int fd, void *buf, int n);
int      sys_write(int fd, const void *buf, int n);
int      sys_close(int fd);

/* BSD-style sockets, but stripped down. No sockaddr structs — the
 * port is just an int. The fd returned by sys_socket flows through
 * the same fd table as files; sys_read/write/close work on it. */
int      sys_socket(void);
int      sys_bind  (int fd, int port);
int      sys_listen(int fd, int backlog);
int      sys_accept(int fd);
/* Active open. ip is 4 bytes in network order; port is host order.
 * Blocks until ESTABLISHED. Returns 0 on success, -1 on failure. */
int      sys_connect(int fd, const unsigned char ip[4], int port);

/* Process primitives. fork() returns the child pid in the parent and
 * 0 in the child. exec() never returns on success — it replaces the
 * calling process's address space with the loaded ELF and restarts
 * at the new program's _start. wait() blocks until any child exits;
 * if exit_code is non-NULL it receives the child's exit code. */
int      sys_fork (void);
int      sys_exec (const char *path, const char *const *argv);
int      sys_wait (int *exit_code);

/* Non-blocking variant. Returns:
 *    >0  pid of a reaped zombie (exit_code filled if non-NULL)
 *     0  no zombie ready right now (children may still be live)
 *    -1  no children at all
 *
 * Used by fork-per-connection servers to drain zombies between
 * accept() calls without committing to a blocking wait. */
int      sys_wait_nb(int *exit_code);

/* Returns the LAPIC ID of the CPU currently servicing this task's
 * syscall, which is also the CPU the task is running on at this
 * instant. Useful for proving that work has migrated to an AP. */
int      sys_getcpu(void);
int      sys_fbinfo(unsigned int out[4]);
int      sys_smp_stats(unsigned int out[8]);

/* sys_mouse_state and sys_fb_mmap were removed when AdventOS narrowed
 * to a CLI-only OS for developers and AI agents. The kernel slots
 * still exist but return 0. */

/* Queue PCM data for playback on the AC97 codec. Format: 16-bit
 * signed little-endian stereo at 48 kHz. n must be a multiple of
 * 4 (one stereo sample). Returns bytes accepted, or -1 if no AC97
 * device. The buffer is COPIED into kernel staging — caller can
 * reuse on return. */
int      sys_audio_play(const void *pcm, int n);

/* Block-device access. dev_idx 0 is the boot ATA disk; USB drives
 * appear at higher indices once enumerated. SYS_BLOCK_INFO returns
 * the device's block_size + n_blocks + name; READ/WRITE transfer
 * `n` blocks starting at `lba` from/to `buf`. Returns 0/-1.
 * `n` is capped at 32 blocks per call. */
int      sys_block_info (int dev_idx, struct sys_block_info *out);
int      sys_block_read (int dev_idx, unsigned int lba, unsigned int n, void *buf);
int      sys_block_write(int dev_idx, unsigned int lba, unsigned int n, const void *buf);

/* Screen-positioning primitives — used by the vi editor (session 46).
 * AdventOS's VGA / fbcon consoles don't interpret ANSI escapes, so
 * cursor placement + line clearing is exposed directly. */
int      sys_tty_cursor   (int row, int col);
int      sys_tty_clear    (void);
int      sys_tty_clear_eol(void);

/* Process credentials (session 47). uid 0 = root. SYS_SETUID is
 * privileged: only a uid-0 task can switch to a different uid;
 * a non-root task may only "setuid" to its own uid (no-op).
 * Returns 0 / -1. uid/gid inherit across fork and survive exec. */
int      sys_getuid(void);
int      sys_getgid(void);
int      sys_setuid(int uid);
int      sys_setgid(int gid);
/* (uid << 16) | gid for the file at `path`, or -1 if missing.
 * Used by `ls` to display owner info and by `stat`-style tools. */
int      sys_fs_owner(const char *path);

/* Permission bits — low 9 bits = rwxrwxrwx. -1 if missing. */
int      sys_fs_mode (const char *path);

/* Change mode / owner of a disk file (session 48). chmod requires
 * the caller be the file's owner or root; chown requires root.
 * Returns 0 on success, -1 on failure (no perm, path missing). */
int      sys_chmod   (const char *path, int mode);
int      sys_chown   (const char *path, int uid, int gid);

/* Open a fresh pseudo-terminal pair (session 52). Writes the master
 * fd into fds[0] and the slave fd into fds[1]. Bytes written to the
 * master appear on slave's read; bytes written to slave appear on
 * master's read. Used by sshd to set up a real-tty environment for
 * the remote shell. Returns 0 on success, -1 if the pty table is full. */
int      sys_openpty(int fds[2]);

/* ---- Session 57: debugger plumbing ----
 *
 * The fb_takeover / mouse_inject wrappers from this batch were
 * removed with the WM and mouse driver; only kbd_poll and ptrace
 * remain.
 *
 *   kbd_poll()        : non-blocking — returns next keystroke byte or 0.
 *                       Bypasses the cooked TTY layer; useful when a
 *                       CLI program wants edge-triggered keyboard input
 *                       without changing the global TTY mode.
 *   ptrace(op,pid,a)  : ptrace multiplexed syscall — see syscall.h for
 *                       the op set + ptrace_args layout. */
int      sys_kbd_poll    (void);
int      sys_ptrace      (int op, int pid, void *args);
/* Session 60: SNTP + DNS-cache + DHCP-info wrappers. */
int      sys_ntp_sync    (const unsigned char ip[4]);
int      sys_ntp_test_responder(int on, unsigned int epoch);
int      sys_dns_cache_stats(unsigned int out[4]);
int      sys_dhcp_info   (struct sys_dhcp_info *out);
/* Session 62: flip the FD_FL_NONBLOCK bit on a per-fd flags byte.
 * on=1 makes the fd non-blocking — SYS_ACCEPT / SYS_READ on a socket
 * fd will return -1 instead of yielding when nothing is queued. Lets
 * the userspace WM pump many client fds from a single 60-fps loop
 * without ever stalling on any one of them. */
int      sys_fd_nb       (int fd, int on);

/* Session 67: feed `n` bytes through the COM1 RX byte-translation +
 * keyboard_inject pipeline as if they had arrived on the UART. Used
 * by the [t49] selftest to prove the serial-input path; agents and
 * normal programs use sys_read on fd 0 instead. Returns n on success
 * or -1 on bad pointer / n out of range (cap is 256). */
int      sys_serial_inject(const char *bytes, int n);

/* Pipes + redirection plumbing.
 *   pipe(fds): fds[0] = read end, fds[1] = write end
 *   dup2(o,n): atomic close-then-clone, n becomes a copy of o
 *   open_w   : create-or-truncate a writable in-RAM file (the > target) */
int      sys_pipe   (int fds[2]);
int      sys_dup2   (int oldfd, int newfd);
int      sys_open_w (const char *name);

/* Signals.
 *   kill(pid, sig)            queues sig in pid's pending set
 *   sigaction(sig, h)         installs h (or SIG_DFL/SIG_IGN); returns previous
 *   signal(sig, h)            POSIX-flavored alias for sigaction()
 * The libuser-internal _sigreturn_tramp symbol is passed to the kernel
 * automatically; user code never sees it. */
int          sys_kill (int pid, int sig);
sighandler_t sigaction(int sig, sighandler_t handler);
sighandler_t signal   (int sig, sighandler_t handler);

/* Heap.
 *   sys_brk(0)      query current break (returns user-VA of the
 *                   first byte past the heap)
 *   sys_brk(new)    set break to `new`. Returns the actual break;
 *                   != requested means failure (out-of-range or
 *                   out-of-memory). The heap lives in a fixed
 *                   per-process VA window; first call extends from
 *                   USER_HEAP_START upward by mapping fresh pages.
 *   malloc/free     a free-list allocator that grows the heap via
 *                   sys_brk on demand. Same shape as kernel kmalloc:
 *                   16-byte aligned blocks, split-on-alloc,
 *                   neighbor-coalesce on free.
 */
int      sys_brk(int new_brk);
void    *malloc (size_t size);
void     free   (void *p);

/* TTY (session 18). The mode is a single bitmap of TTY_ICANON +
 * TTY_ECHO. set returns the previous flags so callers can save and
 * restore. The default at boot is TTY_DEFAULT (= TTY_ICANON|TTY_ECHO).
 * tty_inject() pushes bytes into the keyboard input ring as if they
 * had been typed — used for tests and for headless boots that have
 * no real input source. */
uint32_t tty_set_mode(uint32_t flags);
uint32_t tty_get_mode(void);
int      tty_inject  (const char *bytes, int n);

/* Persist `n` bytes as the entire contents of file `name` on the
 * on-disk AdventFS. Creates the file if it doesn't exist; replaces
 * any prior contents. Returns 0 on success, -1 on out-of-disk or
 * out-of-slot. The new contents survive a reboot. */
int      sys_fs_write(const char *name, const void *data, uint32_t n);

/* Job control + process groups + sessions (session 20). All of these
 * mirror their POSIX counterparts; pid==0 means "the calling task"
 * for the get/set helpers. setsid() makes the caller a session and
 * pgrp leader (sid = pgid = pid) and returns the new sid.
 *   killpg(pgid, sig) broadcasts to every task with matching pgid.
 *   tcsetpgrp(fd, pgid) / tcgetpgrp(fd) set/read the foreground pgrp
 *   on the controlling tty (we have one tty so fd is ignored).
 */
int      setpgid  (int pid, int pgid);
int      getpgid  (int pid);
int      setsid   (void);
int      getsid   (int pid);
int      killpg   (int pgid, int sig);
int      tcsetpgrp(int fd, int pgid);
int      tcgetpgrp(int fd);

/* DNS A-record lookup. Blocks until the kernel's DNS resolver gets
 * a reply or times out. Writes the IPv4 address into the 4-byte
 * buffer at `ip`. Returns 0 on success, -1 on timeout / parse fail
 * / DNS not configured. */
int      sys_dns_resolve(const char *name, unsigned char ip[4]);

/* Free sectors remaining in the AdventFS area. Reads the kernel's
 * bitmap directly — useful for tests that want to verify rewrites
 * actually reuse old sectors instead of leaking them. */
uint32_t sys_fs_free_sectors(void);

/* Map a file region into our address space. Returns a user VA on
 * success or NULL on failure (out of mmap window / no slot / not an
 * FS fd). The pages are NOT backed by physical memory until first
 * touch — the kernel's #PF handler reads the file slice on demand.
 * Behaves like POSIX mmap(NULL, length, PROT_READ|PROT_WRITE,
 * MAP_PRIVATE, fd, offset) — writes don't propagate back to disk. */
void    *sys_mmap   (int fd, uint32_t offset, uint32_t length);
int      sys_munmap (void *addr, uint32_t length);

/* Hierarchical-FS plumbing (session 25). Path semantics match POSIX
 * conventions: leading `/` = absolute, otherwise relative to the
 * task's cwd. mkdir requires the parent path to already exist.
 * sys_readdir takes a directory path and an opaque iterator (start
 * with *iter = 0); each call returns the next entry's index and
 * writes its name (NUL-padded to 16 bytes) into name_buf. -1 ends. */
int      sys_mkdir   (const char *path);
int      sys_chdir   (const char *path);
int      sys_getcwd  (char *buf, int cap);
int      sys_readdir (const char *dir_path, int *iter, char *name_buf);

/* Diagnostics — read internal allocator state for tests / heap dumps. */
uint32_t malloc_total(void);     /* bytes managed (g_brk - HEAP_START) */
uint32_t malloc_used (void);     /* bytes in non-free blocks */
uint32_t malloc_free_bytes(void);/* bytes in free blocks */
uint32_t malloc_brk(void);       /* current break VA */

/* Block cache plumbing (session 27). bcache_sync forces every dirty
 * block out to disk and returns the number of writes issued. The
 * stats array gets filled with cumulative counters:
 *   out[0] = hits           cache lookups served without disk I/O
 *   out[1] = misses         cache lookups that triggered an ata_read
 *   out[2] = logical_writes total calls to bcache_write
 *   out[3] = disk_writes    actual ata_write_sector calls (sync + evict)
 *   out[4] = dirty          live count of dirty cache slots right now
 */
uint32_t sys_bcache_sync (void);
int      sys_bcache_stats(uint32_t out[5]);

void     putchar(char c);
void     puts(const char *s);
void     printf(const char *fmt, ...);

size_t   strlen(const char *s);
int      strcmp(const char *a, const char *b);
int      strncmp(const char *a, const char *b, size_t n);
void    *memset(void *p, int c, size_t n);
void    *memcpy(void *d, const void *s, size_t n);
int      memcmp(const void *a, const void *b, size_t n);

/* Common C-library bits used by the coreutils sweep (session 26). */
int          atoi  (const char *s);
const char  *strchr(const char *s, int c);

#endif
