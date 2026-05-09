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

typedef unsigned int  uint32_t;
typedef int           int32_t;
typedef unsigned int  size_t;

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

void     putchar(char c);
void     puts(const char *s);
void     printf(const char *fmt, ...);

size_t   strlen(const char *s);
int      strcmp(const char *a, const char *b);
int      strncmp(const char *a, const char *b, size_t n);
void    *memset(void *p, int c, size_t n);
void    *memcpy(void *d, const void *s, size_t n);

#endif
