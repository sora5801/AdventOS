#ifndef ADVENTOS_SIGNAL_H
#define ADVENTOS_SIGNAL_H

#include "../include/types.h"

/*
 * Unix-flavored signals. Per-task pending bitmap + mask bitmap +
 * handler table. Delivery happens at the end of syscall_dispatch()
 * and irq_handler() when those routines are about to iret back to
 * ring 3: the kernel builds a delivery frame on the user stack
 * (full saved register state + sig number + trampoline return addr)
 * and rewrites r->eip / r->useresp so iret lands in the user handler.
 *
 * The handler returns to a libuser-provided trampoline that issues
 * SYS_SIGRETURN. Sigreturn reads the saved frame off the user stack,
 * overwrites the kernel's saved registers, and returns — the next
 * iret resumes the task at its pre-signal EIP/ESP.
 */

#define NSIG       32

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGABRT     6
#define SIGFPE      8
#define SIGKILL     9   /* uncatchable: always terminates */
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGCHLD    17

/* User-facing handler-pointer sentinels. Kept as void* so we can
 * stuff them through the same array slot as real function pointers. */
#define SIG_DFL    ((void *)0)
#define SIG_IGN    ((void *)1)

struct registers;       /* from isr.h */
struct task;            /* from task.h */

/* Called per signal-aware syscall return / IRQ return when about to
 * iret to ring 3. If the current task has a deliverable signal, this
 * mutates r->eip / r->useresp / etc. so the next iret jumps into the
 * user handler. Idempotent — safe to call when nothing is pending. */
void signal_check_and_deliver(struct registers *r);

/* SYS_SIGRETURN dispatcher. Reads the saved frame off the user stack
 * (at r->useresp) and overwrites r so the next iret returns to the
 * pre-signal context. */
void signal_sigreturn(struct registers *r);

/* SYS_KILL: queue `sig` in `target_pid`'s pending set. Returns 0 on
 * success, -1 on bad pid / bad sig. */
int  signal_send(uint32_t target_pid, int sig);

/* SYS_SIGACTION: install `handler` for `sig` in the calling task.
 * `tramp` is the user-VA of the libuser sigreturn trampoline; it gets
 * stashed once (overwritten on each call, but every libuser caller
 * passes the same address so this is fine). Returns previous handler. */
void *signal_install(int sig, void *handler, void *tramp);

/* Initialize a task's signal state. Called from task_create. */
void signal_init_task(struct task *t);

/* Reset handlers to SIG_DFL on exec — POSIX semantics. The mask
 * carries over; pending stays. */
void signal_reset_on_exec(struct task *t);

#endif
