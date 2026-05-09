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
