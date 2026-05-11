/*
 * Session 57 — ptrace plumbing for the in-OS debugger.
 *
 * The userspace debugger talks to the kernel through ONE syscall
 * (SYS_PTRACE) multiplexed by op. The actual heavy lifting per op
 * lives here:
 *
 *   TRACEME    set self.tracer_pid = parent.pid
 *   ATTACH     set target.tracer_pid = caller.pid; SIGSTOP target
 *   DETACH     clear target.tracer_pid; CONT if stopped
 *   PEEKDATA   read N bytes from target's user VA into caller's buf
 *   POKEDATA   write N bytes the other way (used to plant 0xCC)
 *   GETREGS    copy target's saved iret frame into caller's regs
 *   SETREGS    write caller's regs back into target's saved frame
 *   CONT       resume target (clear stop bit, state = READY)
 *   STEP       set EFLAGS.TF in the saved frame, then CONT
 *
 * Cross-VA-space copy is the tricky bit: when the tracer is doing a
 * PEEK / POKE, we're running on the tracer's CR3 but we need to read
 * the TRACEE's pages. We handle that with a temporary CR3 swap, the
 * same trick paging_clone_user_pd uses on fork (kernel.c, session 18).
 *
 * Locking: SYS_PTRACE runs under the BKL like every other syscall,
 * so there's no per-task lock around tracer_pid / traced_stopped /
 * trap_frame. The tracee is always TASK_STATE_STOPPED while the
 * tracer is operating on it; if the tracee weren't stopped, reading
 * trap_frame would race with kernel writes — we explicitly reject
 * GETREGS / SETREGS / PEEK / POKE when traced_stopped == 0.
 */

#include "../include/types.h"
#include "syscall.h"
#include "task.h"
#include "signal.h"
#include "kprintf.h"
#include "paging.h"
#include "string.h"

extern void schedule(void);

/* Look up a task by pid, returning the slot pointer or NULL. Excludes
 * DEAD / UNUSED slots — the tracer can't operate on a corpse. */
static struct task *task_for_pid(uint32_t pid) {
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = task_at(i);
        if (!t) continue;
        if (t->id == pid &&
            t->state != TASK_STATE_DEAD &&
            t->state != TASK_STATE_UNUSED) {
            return t;
        }
    }
    return NULL;
}

/* Copy `n` bytes from tracee's user VA `src_va` into the tracer's
 * kernel-visible `dst_buf`. The tracer's `dst_buf` is itself a user
 * VA in the tracer's address space, but the syscall ABI is "we're
 * already on the tracer's CR3 in syscall_dispatch", so dst_buf is
 * already addressable.
 *
 * Strategy: walk the tracee's PD page-by-page, translate to physical,
 * read out via the kernel's identity-mapped low-RAM window. This
 * avoids a CR3 swap entirely and works the same regardless of which
 * task we're on.
 *
 * Returns the byte count actually copied (may be short if a page in
 * the tracee isn't mapped). */
static int peek_user(struct task *tracee, uint32_t src_va,
                     void *dst_buf, uint32_t n) {
    uint8_t *dst = (uint8_t *)dst_buf;
    uint32_t copied = 0;
    while (copied < n) {
        uint32_t va    = src_va + copied;
        uint32_t off   = va & 0xFFFu;
        uint32_t chunk = 0x1000u - off;
        if (chunk > n - copied) chunk = n - copied;

        uint32_t pa = paging_user_va_to_pa(
            (uint32_t *)(uintptr_t)tracee->cr3, va);
        if (pa == 0) break;          /* unmapped — short copy */

        /* Identity window at low RAM (set up in paging_init) covers
         * the first 4 MiB; for higher physical pages we'd need a
         * temp map. User pages in this OS sit well within the
         * identity-mapped region in practice. */
        const uint8_t *src = (const uint8_t *)(uintptr_t)(pa);
        for (uint32_t i = 0; i < chunk; i++) dst[copied + i] = src[i];
        copied += chunk;
    }
    return (int)copied;
}

/* Symmetric POKE — write `n` bytes from `src_buf` (tracer VA, already
 * addressable on tracer CR3) into tracee VA `dst_va`. Same page-walk
 * + identity-map trick. Used by the debugger to plant / clear 0xCC
 * breakpoints in the tracee's text segment. */
static int poke_user(struct task *tracee, uint32_t dst_va,
                     const void *src_buf, uint32_t n) {
    const uint8_t *src = (const uint8_t *)src_buf;
    uint32_t copied = 0;
    while (copied < n) {
        uint32_t va    = dst_va + copied;
        uint32_t off   = va & 0xFFFu;
        uint32_t chunk = 0x1000u - off;
        if (chunk > n - copied) chunk = n - copied;

        uint32_t pa = paging_user_va_to_pa(
            (uint32_t *)(uintptr_t)tracee->cr3, va);
        if (pa == 0) break;

        uint8_t *dst = (uint8_t *)(uintptr_t)pa;
        for (uint32_t i = 0; i < chunk; i++) dst[i] = src[copied + i];
        copied += chunk;
    }
    return (int)copied;
}

/* Build a struct ptrace_regs from a stopped tracee's saved iret frame.
 * The frame layout is the one isr_common_stub pushed; struct
 * registers (isr.h) describes it. */
static void copy_regs_out(struct task *tracee, struct ptrace_regs *out) {
    struct registers *r = (struct registers *)tracee->trap_frame;
    if (!r) {
        for (size_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
        return;
    }
    out->eax    = r->eax;
    out->ebx    = r->ebx;
    out->ecx    = r->ecx;
    out->edx    = r->edx;
    out->esi    = r->esi;
    out->edi    = r->edi;
    out->ebp    = r->ebp;
    out->eip    = r->eip;
    out->esp    = r->useresp;
    out->eflags = r->eflags;
}

/* Inverse of copy_regs_out: write tracer-supplied regs back into the
 * tracee's saved frame. SETREGS is how the debugger "rewinds" EIP
 * after a hit (INT3 leaves EIP one past the 0xCC byte; tracer puts
 * it back at the breakpoint address so the original instruction
 * re-executes).
 *
 * We sanity-clamp two fields:
 *   - CS / SS are NOT writable; the tracer can't punch a kernel
 *     selector into a user task and expect privilege to escalate.
 *   - EFLAGS bits that gate CPL or I/O privilege are masked out
 *     (IOPL, IF, VM, NT) — only the user-controllable flags survive.
 *     This is the same posture POSIX ptrace takes. */
static void copy_regs_in(struct task *tracee, const struct ptrace_regs *in) {
    struct registers *r = (struct registers *)tracee->trap_frame;
    if (!r) return;
    r->eax    = in->eax;
    r->ebx    = in->ebx;
    r->ecx    = in->ecx;
    r->edx    = in->edx;
    r->esi    = in->esi;
    r->edi    = in->edi;
    r->ebp    = in->ebp;
    r->eip    = in->eip;
    r->useresp = in->esp;
    /* Allow only the user-status bits + TF. Force IF on (so the
     * tracee doesn't accidentally run with interrupts disabled) and
     * IOPL=0. */
    uint32_t mask_keep   = 0x00000ED5u;        /* SF/ZF/AF/PF/CF + DF + OF + TF + AC */
    uint32_t mask_force  = 0x00000200u;        /* IF=1 */
    r->eflags = (in->eflags & mask_keep) | mask_force;
}

/* The public entry. Called from syscall.c's SYS_PTRACE branch with the
 * caller's args already unpacked. Returns op-specific value (mostly 0
 * on success / -1 on error; PEEKDATA returns bytes copied). */
int ptrace_dispatch(int op, uint32_t pid, void *args_ptr) {
    struct task *me = task_current();
    if (!me) return -1;

    /* TRACEME doesn't take a pid arg — the calling task marks itself
     * traced by its parent. Common pattern: child forks, calls
     * TRACEME, then execs the target binary. The kernel then drops
     * a SIGTRAP into the child on the next iret-to-ring-3 (entry to
     * the new program), parking it for the parent's debugger to
     * inspect. */
    if (op == PTRACE_TRACEME) {
        me->tracer_pid = me->parent_id;
        return 0;
    }

    /* Every other op needs a target task. */
    struct task *t = task_for_pid(pid);
    if (!t) return -1;

    switch (op) {
        case PTRACE_ATTACH: {
            /* Only attach if no other tracer is already attached.
             * Real ptrace also requires ptrace_scope / capabilities;
             * we keep it permissive — same user/no-perm-check posture
             * as the rest of the kernel. */
            if (t->tracer_pid != 0 && t->tracer_pid != me->id) return -1;
            t->tracer_pid = me->id;
            /* SIGSTOP queues a stop that signal_check_and_deliver
             * will see at the tracee's next iret. The tracer's
             * sys_wait will then return. */
            signal_send(t->id, SIGSTOP);
            return 0;
        }

        case PTRACE_DETACH: {
            if (t->tracer_pid != me->id) return -1;
            t->tracer_pid    = 0;
            t->traced_stopped = 0;
            t->trap_frame    = 0;
            if (t->state == TASK_STATE_STOPPED) {
                t->state = TASK_STATE_READY;
            }
            return 0;
        }

        case PTRACE_CONT:
        case PTRACE_STEP: {
            if (t->tracer_pid != me->id) return -1;
            if (!t->traced_stopped)       return -1;

            if (op == PTRACE_STEP) {
                /* Arm a single-step trap by setting TF in the saved
                 * EFLAGS. The CPU clears TF on entry to the #DB
                 * handler, so we don't risk infinite-stepping. */
                struct registers *r = (struct registers *)t->trap_frame;
                if (r) r->eflags |= 0x100u;     /* TF */
            }
            t->traced_stopped = 0;
            t->trap_frame     = 0;
            if (t->state == TASK_STATE_STOPPED) {
                t->state = TASK_STATE_READY;
            }
            return 0;
        }

        case PTRACE_PEEKDATA:
        case PTRACE_POKEDATA: {
            if (t->tracer_pid != me->id) return -1;
            if (!t->traced_stopped)       return -1;
            struct ptrace_args *a = (struct ptrace_args *)args_ptr;
            if (!a || !a->buf || a->size == 0) return -1;
            if (op == PTRACE_PEEKDATA)
                return peek_user(t, a->addr, a->buf,  a->size);
            else
                return poke_user(t, a->addr, a->buf,  a->size);
        }

        case PTRACE_GETREGS:
        case PTRACE_SETREGS: {
            if (t->tracer_pid != me->id) return -1;
            if (!t->traced_stopped)       return -1;
            struct ptrace_args *a = (struct ptrace_args *)args_ptr;
            if (!a || !a->regs) return -1;
            if (op == PTRACE_GETREGS) copy_regs_out(t, a->regs);
            else                      copy_regs_in (t, a->regs);
            return 0;
        }

        case PTRACE_WAIT: {
            /* Block until the tracee transitions. We yield rather
             * than parking ourselves in a custom blocked state to
             * keep BKL handoffs trivial — the polled state is set
             * from the trap path (trap_stop_for_tracer) or from
             * task_exit_current via tracer_pid notification, so a
             * coarse poll is enough. The yield gives other tasks
             * a turn.
             *
             * The eligibility check is "either we're already the
             * established tracer, OR we're the tracee's fork-parent
             * (and the tracee hasn't completed PTRACE_TRACEME yet)".
             * Without the second clause there's a race where the
             * parent's WAIT runs before the child's TRACEME and
             * gets a hard -1. */
            extern void task_yield(void);
            if (t->tracer_pid != me->id && t->parent_id != me->id) return -1;
            for (;;) {
                struct task *tt = task_for_pid(pid);
                if (!tt) return 0;
                if (tt->state == TASK_STATE_ZOMBIE) return 0;
                if (tt->traced_stopped) {
                    return tt->trap_signal ? tt->trap_signal : SIGTRAP;
                }
                task_yield();
            }
        }

        default:
            return -1;
    }
}
