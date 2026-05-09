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

/* Process primitives. fork() returns the child pid in the parent and
 * 0 in the child. exec() never returns on success — it replaces the
 * calling process's address space with the loaded ELF and restarts
 * at the new program's _start. wait() blocks until any child exits;
 * if exit_code is non-NULL it receives the child's exit code. */
int      sys_fork (void);
int      sys_exec (const char *path, const char *const *argv);
int      sys_wait (int *exit_code);

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

/* Diagnostics — read internal allocator state for tests / heap dumps. */
uint32_t malloc_total(void);     /* bytes managed (g_brk - HEAP_START) */
uint32_t malloc_used (void);     /* bytes in non-free blocks */
uint32_t malloc_free_bytes(void);/* bytes in free blocks */
uint32_t malloc_brk(void);       /* current break VA */

void     putchar(char c);
void     puts(const char *s);
void     printf(const char *fmt, ...);

size_t   strlen(const char *s);
int      strcmp(const char *a, const char *b);
int      strncmp(const char *a, const char *b, size_t n);
void    *memset(void *p, int c, size_t n);
void    *memcpy(void *d, const void *s, size_t n);

#endif
