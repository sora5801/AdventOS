#ifndef ADVENTOS_SYSCALL_H
#define ADVENTOS_SYSCALL_H

#include "../include/types.h"
#include "isr.h"

/* Syscall numbers — kept tiny for the demo. */
#define SYS_WRITE      1   /* (eax=1, ebx=ch)         -> writes one char        */
#define SYS_GETPID     2   /* (eax=2)                 -> pid in eax             */
#define SYS_EXIT       3   /* (eax=3, ebx=code)       -> never returns          */
#define SYS_YIELD      4   /* (eax=4)                 -> calls schedule()       */
#define SYS_WRITE_STR  5   /* (eax=5, ebx=ptr)        -> writes a NUL-term str  */
#define SYS_SLEEP_MS   6   /* (eax=6, ebx=ms)         -> sleeps + reschedules   */
#define SYS_TIME       7   /* (eax=7)                 -> UNIX epoch in eax      */
#define SYS_READ_LINE  8   /* (eax=8, ebx=buf, ecx=n) -> read a line, len in eax*/
/* SYS_KCMD = 9 retired in session 14 — userspace shell uses fork/exec/wait now. */
#define SYS_OPEN       10  /* (eax=10, ebx=name)         -> fd or -1            */
#define SYS_READ       11  /* (eax=11, ebx=fd, ecx=buf, edx=n) -> bytes or 0/-1 */
#define SYS_WRITE_FD   12  /* (eax=12, ebx=fd, ecx=buf, edx=n) -> bytes or -1   */
#define SYS_CLOSE      13  /* (eax=13, ebx=fd)           -> 0 or -1             */
#define SYS_SOCKET     14  /* (eax=14)                   -> fd or -1            */
#define SYS_BIND       15  /* (eax=15, ebx=fd, ecx=port) -> 0 or -1             */
#define SYS_LISTEN     16  /* (eax=16, ebx=fd, ecx=back) -> 0 or -1             */
#define SYS_ACCEPT     17  /* (eax=17, ebx=fd)           -> conn fd or -1       */
#define SYS_FORK       18  /* (eax=18)                   -> child pid (parent), 0 (child), -1 err */
#define SYS_EXEC       19  /* (eax=19, ebx=path, ecx=argv) -> -1 on err, no return on success */
#define SYS_WAIT       20  /* (eax=20, ebx=&exit_code)   -> child pid or -1 if no children */
#define SYS_PIPE       21  /* (eax=21, ebx=&fds[2])      -> 0 / -1; fds[0]=read end fds[1]=write end */
#define SYS_DUP2       22  /* (eax=22, ebx=oldfd, ecx=newfd) -> newfd or -1 */
#define SYS_OPEN_W     23  /* (eax=23, ebx=name)         -> tmpfs fd (truncated) or -1 */
#define SYS_KILL       24  /* (eax=24, ebx=pid, ecx=sig) -> 0 or -1 */
#define SYS_SIGACTION  25  /* (eax=25, ebx=sig, ecx=handler, edx=tramp) -> previous handler */
#define SYS_SIGRETURN  26  /* (eax=26)                   -> never returns to caller; restores ctx */

void syscall_dispatch(struct registers *r);

#endif
