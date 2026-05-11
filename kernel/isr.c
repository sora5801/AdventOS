#include "isr.h"
#include "kprintf.h"
#include "pic.h"
#include "syscall.h"
#include "signal.h"
#include "mmap.h"
#include "lapic.h"
#include "task.h"
#include "smp.h"
#include "../include/io.h"

static const char *exception_names[32] = {
    "Divide-by-zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "Bound range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved",
    "x87 FPU error",
    "Alignment check",
    "Machine check",
    "SIMD FP exception",
    "Virtualization exception",
    "Control protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
};

static irq_handler_fn irq_handlers[16];

void isr_register_irq(int irq, irq_handler_fn handler) {
    if (irq >= 0 && irq < 16) irq_handlers[irq] = handler;
}

/* Stop the current task for a tracer (session 57). Called from the
 * INT3 / #DB exception paths when the trap originated in ring 3.
 *
 * The pattern is straight ptrace: park `trap_frame` so the tracer can
 * GETREGS / SETREGS off it, mark traced_stopped, deliver SIGCHLD to
 * the tracer so its sys_wait wakes up, flip our state, schedule away.
 * When the tracer later calls PTRACE_CONT / STEP, it flips us back
 * to READY and the scheduler resumes us inside the iret tail — the
 * pre-trap context the CPU pushed is what we iret back to. */
static void trap_stop_for_tracer(struct registers *r, int trap_signal) {
    extern void schedule(void);
    extern int  bkl_held(void);
    extern void bkl_lock(void);
    extern void bkl_unlock(void);

    struct task *t = task_current();
    if (!t || !t->is_user || t->tracer_pid == 0) {
        /* No tracer (or kernel-mode trap) — fall through to default
         * diagnostic. The caller decides whether to halt or kill. */
        return;
    }

    t->trap_frame    = r;
    t->trap_signal   = trap_signal;
    t->traced_stopped = 1;
    t->state         = TASK_STATE_STOPPED;

    /* Wake the tracer if it was blocked in sys_wait. signal_send
     * sets the SIGCHLD pending bit; the wait machinery checks for
     * stopped children too. */
    signal_send(t->tracer_pid, SIGCHLD);

    /* Same BKL handoff dance as signal_check_and_deliver's ACT_STOP:
     * the syscall tail won't run, so we must release before
     * scheduling and re-take afterward when the tracer resumes us. */
    int had_bkl = bkl_held();
    if (had_bkl) bkl_unlock();
    schedule();
    if (had_bkl) bkl_lock();
}

/*
 * Called from isr_common_stub for CPU exceptions.
 * The pointer is passed via cdecl on the stack; on mingw32 the symbol
 * needs the leading underscore handled by the assembler.
 */
void isr_handler(struct registers *r) {
    uint32_t n = r->int_no;

    if (n == 128) {
        syscall_dispatch(r);
        return;
    }

    if (n < 32) {
        /* Debugger traps (session 57): INT3 (#BP, vector 3) is a
         * software breakpoint hit; #DB (vector 1) is a single-step
         * trap raised by the CPU after EFLAGS.TF=1 retired one
         * instruction. Both stop the tracee if there's a tracer
         * attached; otherwise we fall through to the default
         * "kill the task with SIGSEGV-style halt" diagnostic. */
        if ((n == 1 || n == 3) && (r->cs & 0x3) == 3) {
            struct task *t = task_current();
            if (t && t->tracer_pid != 0) {
                /* #DB is delivered as a Fault (re-execute the
                 * instruction with TF cleared)? In our model both
                 * INT3 and TF-step are reported to the tracer as
                 * SIGTRAP. TF is auto-cleared by the CPU on entry
                 * to the handler. */
                if (n == 1) {
                    /* CPU already cleared TF in the saved EFLAGS
                     * snapshot, but be defensive — some emulators
                     * differ. */
                    r->eflags &= ~0x100u;
                }
                trap_stop_for_tracer(r, SIGTRAP);
                return;
            }
        }

        /* Page fault graduates from defensive panic to productive
         * lazy-loader (session 24). If cr2 lands in any of the
         * current task's mmap regions, the handler allocates a fresh
         * page, fs_reads the file slice into it, maps it, returns.
         * Otherwise we fall through to the existing diagnostic +
         * halt so unhandled faults still surface loudly. */
        if (n == 14) {
            uint32_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            if (mmap_handle_fault(r, cr2) == 0) {
                return;
            }
        }

        kprintf("\n[!] CPU EXCEPTION %u: %s (err=0x%x) at %x:%x  eflags=0x%x\n",
                (unsigned)n,
                exception_names[n],
                (unsigned)r->err_code,
                (unsigned)r->cs,
                (unsigned)r->eip,
                (unsigned)r->eflags);
        if (n == 14) {
            uint32_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            uint32_t e = r->err_code;
            kprintf("    fault addr (CR2) = 0x%08x\n", (unsigned)cr2);
            kprintf("    cause = %s, %s, %s%s%s\n",
                    (e & 1) ? "protection violation" : "page not present",
                    (e & 2) ? "write" : "read",
                    (e & 4) ? "user mode" : "supervisor mode",
                    (e & 8) ? ", reserved-bit set" : "",
                    (e & 16) ? ", instruction fetch" : "");
        }
        kputs("System halted.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
}

/*
 * Called from irq_common_stub. We send EOI BEFORE invoking the
 * registered handler — the timer handler may context-switch and
 * never return, in which case a deferred EOI would silently mute
 * further IRQs from that line.
 */
void irq_handler(struct registers *r) {
    int irq = (int)r->int_no - 32;
    if (irq >= 0 && irq < 16) {
        pic_send_eoi(irq);
        if (irq_handlers[irq]) irq_handlers[irq](r);
    }

    /* If we're about to iret back to ring 3, this is a chance to
     * deliver any signals that piled up while the user task was
     * preempted. signal_check_and_deliver checks (r->cs & 3) == 3
     * itself, so kernel-mode preemption is a no-op. */
    signal_check_and_deliver(r);
}

/*
 * LAPIC-timer ISR. Fires on EVERY CPU independently — the LAPIC is
 * per-CPU silicon, so each call is on the local CPU's stack with
 * the local CPU's register state. EOI goes to the LAPIC, not the
 * PIC; signal delivery + schedule() then runs as for the PIT path.
 *
 * Like the PIT handler, the EOI is sent BEFORE schedule() because
 * schedule() may context-switch and never return — a deferred EOI
 * would mute further timer ticks on this CPU.
 */
volatile uint32_t g_lapic_tick_count[8];
volatile uint32_t g_cpu_dispatch[8];

void lapic_irq_handler(struct registers *r) {
    lapic_eoi();

    /* Per-CPU tick counter for diagnostics. Read by `tasks` shell
     * command and the SMP selftest. */
    extern volatile int g_smp_ready;
    if (g_smp_ready) {
        struct cpu_local *cpu = cpu_local();
        if (cpu->cpu_id < 8) g_lapic_tick_count[cpu->cpu_id]++;
    }

    /* Round-robin preemption on every CPU. The schedule() call
     * itself takes the global scheduler lock so concurrent ticks
     * on different CPUs serialize cleanly. */
    schedule();

    /* Same signal-delivery hook as the PIT path — fires only when
     * we're returning to ring 3 (signal_check_and_deliver gates on
     * (r->cs & 3) == 3 itself). */
    signal_check_and_deliver(r);
}
