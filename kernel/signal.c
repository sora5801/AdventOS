#include "signal.h"
#include "task.h"
#include "isr.h"
#include "kprintf.h"
#include "string.h"

/*
 * Layout of the user-stack delivery frame, from low VA to high VA:
 *
 *   [ trampoline_ret  ]  ← user ESP on entry to handler
 *   [ sig_num         ]
 *   [ sigcontext: 64B ]  ← struct registers verbatim
 *
 * On entry to the handler:
 *   - cdecl says ret addr at [ESP+0], arg1 at [ESP+4]. Our stack
 *     has tramp_addr at [ESP+0] and sig_num at [ESP+4]. Handler
 *     does its work, then `ret`s — popping tramp_addr.
 *   - After ret, ESP points at sig_num. Trampoline does
 *     `add $4, %esp` to skip past it, then INT $0x80 SIGRETURN.
 *   - Sigreturn sees user ESP pointing exactly at the sigcontext,
 *     copies it back into the kernel's saved registers, returns —
 *     the next iret restores the pre-signal context byte-for-byte.
 */

/* Default action for each signal. */
enum { ACT_TERM, ACT_IGN, ACT_STOP, ACT_CONT };

static int default_action(int sig) {
    switch (sig) {
        case SIGCHLD:
            return ACT_IGN;     /* parents that don't wait shouldn't die */
        case SIGSTOP:
        case SIGTSTP:
            return ACT_STOP;
        case SIGCONT:
            return ACT_CONT;    /* default is "no-op past the wakeup" */
        default:
            return ACT_TERM;
    }
}

void signal_init_task(struct task *t) {
    t->sig_pending = 0;
    t->sig_mask    = 0;
    t->sig_tramp   = 0;
    for (int i = 0; i < NSIG; i++) t->sig_handlers[i] = SIG_DFL;
}

void signal_reset_on_exec(struct task *t) {
    /* POSIX: catchable handlers reset to SIG_DFL across exec; SIG_IGN
     * stays. The mask and pending set carry over. We ignore SIG_KILL
     * and SIG_STOP entries because we don't have STOP/CONT yet. */
    for (int i = 0; i < NSIG; i++) {
        if (t->sig_handlers[i] != SIG_IGN) {
            t->sig_handlers[i] = SIG_DFL;
        }
    }
    t->sig_tramp = 0;
}

void *signal_install(int sig, void *handler, void *tramp) {
    if (sig < 1 || sig >= NSIG)         return (void *)-1;
    if (sig == SIGKILL || sig == SIGSTOP) return (void *)-1;  /* uncatchable */
    struct task *t = task_current();
    void *prev = t->sig_handlers[sig];
    t->sig_handlers[sig] = handler;
    if (tramp) t->sig_tramp = tramp;
    return prev;
}

int signal_send(uint32_t target_pid, int sig) {
    if (sig < 1 || sig >= NSIG) return -1;
    if (target_pid == 0)        return -1;     /* don't signal kmain */

    struct task *t = NULL;
    /* Session 82 followup: scan ALL TASK_MAX slots, not the literal 16.
     * This was the same latent bug `signal_send_pgrp` already called
     * out below — TASK_MAX bumped 16→32 in session 50 but signal_send
     * was missed. Symptom: agentd's shell.job.cancel returns success
     * but SIGKILL silently dropped for pids whose task slot index
     * ended up past 16 (common after sandbox/limits/kv selftests have
     * allocated their share of the slot table). That left the
     * `/sleep.elf 30` sleeper jobs from jobs-selftest running to
     * natural completion — 30 s instead of the expected ~100 ms —
     * which leaked job-table slots into cron-selftest and produced
     * the recurring cron-selftest 6-FAIL cascade. */
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        struct task *cand = task_at(i);
        if (cand && cand->id == target_pid) { t = cand; break; }
    }
    if (!t) return -1;

    /* SIGCONT clears any pending stop and (if currently STOPPED)
     * wakes the task so it gets scheduled again. The handler / default
     * action runs when the task next iret's to ring 3. */
    if (sig == SIGCONT) {
        t->sig_pending &= ~((1u << SIGSTOP) | (1u << SIGTSTP));
        if (t->state == TASK_STATE_STOPPED) {
            t->state = TASK_STATE_READY;
        }
    }
    /* Conversely, sending a stop clears any pending CONT so the task
     * doesn't immediately wake itself back up. */
    if (sig == SIGSTOP || sig == SIGTSTP) {
        t->sig_pending &= ~(1u << SIGCONT);
    }

    t->sig_pending |= (1u << sig);
    return 0;
}

int signal_send_pgrp(uint32_t pgid, int sig) {
    if (sig < 1 || sig >= NSIG) return -1;
    if (pgid == 0)              return -1;     /* would hit kernel tasks */

    int delivered = 0;
    /* TASK_MAX was 16 when this was written, then bumped to 32 in
     * session 50; the literal 16 here was a silent latent bug. */
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        struct task *t = task_at(i);
        if (!t)                        continue;
        if (t->pgid != pgid)           continue;
        if (t->state == TASK_STATE_DEAD || t->state == TASK_STATE_ZOMBIE) continue;
        if (signal_send(t->id, sig) == 0) delivered++;
    }
    return delivered > 0 ? 0 : -1;
}

/* Pick the lowest-numbered deliverable (pending and not masked)
 * signal. Returns 0 if none. */
static int pick_pending(struct task *t) {
    uint32_t deliverable = t->sig_pending & ~t->sig_mask;
    if (deliverable == 0) return 0;
    for (int s = 1; s < NSIG; s++) {
        if (deliverable & (1u << s)) return s;
    }
    return 0;
}

void signal_check_and_deliver(struct registers *r) {
    /* Only deliver when about to iret to ring 3. Kernel-mode tasks
     * never receive signals. */
    if ((r->cs & 0x3) != 3) return;

    struct task *t = task_current();
    if (!t || !t->is_user) return;

    int sig = pick_pending(t);
    if (sig == 0) return;

    /* Consume the pending bit before delivery so the handler can
     * re-pend the same signal without us redelivering it on the
     * next iret. */
    t->sig_pending &= ~(1u << sig);

    void *h = t->sig_handlers[sig];

    /* SIG_IGN — drop on the floor. */
    if (h == SIG_IGN) return;

    /* SIG_DFL — apply the default action. */
    if (h == SIG_DFL) {
        int act = default_action(sig);
        if (act == ACT_IGN)  return;
        if (act == ACT_CONT) return;     /* wakeup already happened in signal_send */
        if (act == ACT_STOP) {
            extern int  bkl_held(void);
            extern void bkl_lock(void);
            extern void bkl_unlock(void);
            extern void schedule(void);
            t->state = TASK_STATE_STOPPED;
            /* Same BKL handoff as the ACT_TERM path — schedule() may
             * not return for a long time (SIGCONT-driven wakeup), so
             * we must not hold the BKL across it. Re-take if we held
             * it on entry so the syscall tail's matching bkl_unlock
             * is balanced. */
            int had_bkl = bkl_held();
            if (had_bkl) bkl_unlock();
            schedule();                  /* yield until SIGCONT flips us READY */
            if (had_bkl) bkl_lock();
            /* When CONT'd, control resumes here. Fall through to the
             * iret tail in the calling syscall stub — the task picks
             * up at the same EIP it was about to return to. */
            return;
        }
        /* ACT_TERM: behave as if the task called sys_exit(128+sig).
         *
         * BKL handoff: when this fires inside the syscall tail (the
         * common case), syscall_dispatch is still holding the BKL and
         * the bkl_unlock() at its end will never run because we don't
         * return — schedule() context-switches and this task becomes
         * a zombie. Without releasing here, the lock would leak and
         * the next task on this CPU would spin forever in bkl_lock.
         * From IRQ context bkl_held() is false; the check is safe in
         * both call sites. */
        kprintf("[sig] pid=%u terminated by signal %d\n",
                (unsigned)t->id, sig);
        extern void task_exit_current(int);
        task_exit_current(128 + sig);
        extern int  bkl_held(void);
        extern void bkl_unlock(void);
        if (bkl_held()) bkl_unlock();
        extern void schedule(void);
        schedule();
        for (;;) __asm__ volatile ("hlt");
    }

    /* User handler. Build the delivery frame on the user stack. We're
     * still on the user task's CR3 (this runs at the tail of a
     * syscall / IRQ before iret), so user pointers are dereferenceable. */

    if (!t->sig_tramp) {
        /* Handler installed without a trampoline — drop the signal
         * so we don't iret to nowhere. (Shouldn't happen if libuser
         * is used, but guard regardless.) */
        kprintf("[sig] pid=%u sig %d: no trampoline, dropping\n",
                (unsigned)t->id, sig);
        return;
    }

    uint32_t user_esp = r->useresp;

    /* 1. Copy current saved-registers frame onto user stack as
     *    sigcontext. We write it via the kernel identity map by
     *    dereferencing user pointers directly — same trick the rest
     *    of the kernel uses on user buffers. */
    user_esp -= sizeof(struct registers);
    struct registers *uctx = (struct registers *)(uintptr_t)user_esp;
    *uctx = *r;

    /* 2. Push sig_num below sigcontext. */
    user_esp -= 4;
    *(uint32_t *)(uintptr_t)user_esp = (uint32_t)sig;

    /* 3. Push trampoline addr — handler's "return address". */
    user_esp -= 4;
    *(uint32_t *)(uintptr_t)user_esp = (uint32_t)(uintptr_t)t->sig_tramp;

    /* 4. Mutate the kernel-saved frame so iret enters the handler. */
    r->useresp = user_esp;
    r->eip     = (uint32_t)(uintptr_t)h;
    /* CS / SS / DS / EFLAGS were already set for ring 3 by whatever
     * syscall / IRQ we're on the tail of. The handler runs in the
     * same address space + same selectors. */

    /* 5. Auto-mask this signal during the handler so the same signal
     *    can't recurse. The mask is restored from the saved sigcontext
     *    on sigreturn — but we don't actually save the mask in the
     *    current sigcontext shape, so instead sigreturn unmasks the
     *    delivered signal explicitly. Tracking via a separate field. */
    t->sig_mask |= (1u << sig);
}

void signal_sigreturn(struct registers *r) {
    /* User stack on entry to SIGRETURN points at the saved
     * sigcontext (the trampoline's `add $4, %esp` skipped past
     * sig_num). Copy it back over our own frame so iret restores
     * the pre-signal context. */
    uint32_t user_esp = r->useresp;
    struct registers *uctx = (struct registers *)(uintptr_t)user_esp;

    /* Capture the signal number that was delivered so we can unmask
     * it. It lives at user_esp - 4 (we popped over it in the
     * trampoline, so the bytes are still on the stack right below). */
    int delivered_sig = (int)*(uint32_t *)(uintptr_t)(user_esp - 4);

    /* Overwrite our register frame with the saved one. After this
     * the dispatcher's tail will iret into the saved EIP/ESP/etc.
     * Note: we do NOT write r->eax — the dispatcher would normally
     * write its return value there. Caller (syscall.c) avoids that
     * tail for SIGRETURN. */
    *r = *uctx;

    /* Drop the auto-mask we set when delivering. */
    if (delivered_sig > 0 && delivered_sig < NSIG) {
        struct task *t = task_current();
        t->sig_mask &= ~(1u << delivered_sig);
    }
}
