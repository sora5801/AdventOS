#ifndef ADVENTOS_NETLOCK_H
#define ADVENTOS_NETLOCK_H

/*
 * net_lock — recursive spinlock for the TCP + sock tables (g_tcbs in
 * kernel/tcp.c, g_socks in kernel/sock.c).
 *
 * Background: the BKL (kernel/bkl.c) serialises kernel work between
 * *syscalls*, but interrupts run outside the BKL. The rtl8139 RX IRQ
 * path is rtl_irq → eth_rx → ip_rx → tcp_rx → on_recv / on_connect /
 * on_close, all of which mutate g_tcbs / g_socks. Meanwhile a syscall
 * on a peer CPU is happily doing the same to the same arrays under
 * the BKL. The two CPUs collide on the table state.
 *
 * For loopback specifically there's a secondary path: ip_send calls
 * try_loopback which synchronously calls tcp_rx. Adding agentd as a
 * fourth permanent loopback listener widened the find_tcb_for_seg
 * scan window enough that the race graduated from "rare" to
 * "deterministic deadlock in t20" — see docs/66-smp-loopback-fix.md.
 *
 * Semantics:
 *   - net_lock() takes the inner spinlock_t (CLI on this CPU), or if
 *     this CPU already owns the lock, bumps a depth counter and
 *     returns. Lets the try_loopback recursion (syscall → tcp_send
 *     → ip_send → try_loopback → tcp_rx → spawn → tcp_send → ip_send
 *     → try_loopback → tcp_rx …) re-enter the same lock several
 *     levels deep without deadlocking on itself.
 *   - net_unlock() decrements the depth; the outermost release also
 *     resets `owner_cpu` and unlocks the inner spinlock, which
 *     restores the caller's IF.
 *   - net_lock_owned_by_me() — diagnostic. Returns 1 if this CPU is
 *     the current owner. Used to gate the recursive-entry shortcut
 *     in net_lock itself, plus by assertions in code that "should
 *     be" running under the lock.
 *
 * Lock ordering: BKL > net_lock. BKL is taken first by the syscall
 * dispatcher with IF=1; net_lock is taken later inside the syscall
 * body with IF=0 (via the inner spinlock's CLI). IRQ-driven entries
 * to ip_rx take net_lock directly without holding BKL — that's the
 * whole point of the lock, since IRQs cannot acquire BKL safely.
 *
 * NOT held across task_yield. Code that yields inside net_lock must
 * drop and retake around the yield (same pattern as the BKL handoff
 * in task_yield itself). See sock_read / sock_accept for examples.
 */

void net_lock          (void);
void net_unlock        (void);
int  net_lock_owned_by_me(void);

#endif
