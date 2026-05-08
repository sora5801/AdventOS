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

void syscall_dispatch(struct registers *r);

#endif
