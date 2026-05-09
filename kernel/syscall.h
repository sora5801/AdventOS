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
#define SYS_KCMD       9   /* (eax=9, ebx=ptr)        -> run kernel command     */
#define SYS_OPEN       10  /* (eax=10, ebx=name)         -> fd or -1            */
#define SYS_READ       11  /* (eax=11, ebx=fd, ecx=buf, edx=n) -> bytes or 0/-1 */
#define SYS_WRITE_FD   12  /* (eax=12, ebx=fd, ecx=buf, edx=n) -> bytes or -1   */
#define SYS_CLOSE      13  /* (eax=13, ebx=fd)           -> 0 or -1             */
#define SYS_SOCKET     14  /* (eax=14)                   -> fd or -1            */
#define SYS_BIND       15  /* (eax=15, ebx=fd, ecx=port) -> 0 or -1             */
#define SYS_LISTEN     16  /* (eax=16, ebx=fd, ecx=back) -> 0 or -1             */
#define SYS_ACCEPT     17  /* (eax=17, ebx=fd)           -> conn fd or -1       */

void syscall_dispatch(struct registers *r);

#endif
