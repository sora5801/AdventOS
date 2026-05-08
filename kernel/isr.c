#include "isr.h"
#include "kprintf.h"
#include "pic.h"
#include "syscall.h"
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
}
