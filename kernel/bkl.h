#ifndef ADVENTOS_BKL_H
#define ADVENTOS_BKL_H

/*
 * The Big Kernel Lock — session 38's pragmatic answer to "the
 * syscall surface isn't SMP-safe."
 *
 * Every entry into kernel work that touches any of the following
 * shared subsystems must be wrapped in bkl_lock/bkl_unlock:
 *
 *   - fs.c          (file table, dir cache)
 *   - bcache        (in flight; has its own internal lock too)
 *   - vfs.c         (mount table)
 *   - elf loader    (one shared scratch path)
 *   - paging_clone_user_pd / paging_destroy_user_pd
 *   - sock / pipe / tmpfs / ttys / signal — single-CPU assumptions
 *   - dns, dhcp, tcp — share table state
 *
 * Held across sys_* implementations via syscall_dispatch's wrapper.
 * NOT held across schedule() — bkl_lock callers must drop the lock
 * if they yield (sys_wait, sys_sleep_ms, etc.) so other CPUs can
 * make progress while one task blocks. The blocking-call-yield
 * pattern is:
 *
 *     bkl_unlock();
 *     task_yield();          // or pit_sleep, schedule, ...
 *     bkl_lock();
 *
 * (We could use a cleverer "drop on schedule, re-take on resume"
 * trick like g_sched_lock does, but that requires every yield path
 * to participate. The explicit drop is robust and easy to audit.)
 *
 * This is intentionally coarse. A future session can split it into
 * per-subsystem locks (rwlock for fs metadata, dedicated lock for
 * bcache, dedicated lock for sock table, etc.) — but for our 16-
 * task system the BKL costs essentially nothing in throughput
 * because syscall density is low.
 */

void bkl_lock(void);
void bkl_unlock(void);

/* Returns 1 if currently held by the calling CPU. Used as a
 * defensive assert hook in code that "should be" inside the BKL
 * (e.g., the existing syscall handlers, post-rewrite). */
int  bkl_held(void);

#endif
