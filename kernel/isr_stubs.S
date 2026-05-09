/*
 * ISR / IRQ assembly stubs.
 *
 * Each per-interrupt stub pushes a (fake or real) error code, then the
 * interrupt number, then jumps to a common dispatch point that:
 *   - saves all general registers (pusha) and DS
 *   - reloads kernel data segment
 *   - calls the C dispatch routine with a pointer to the saved frame
 *   - restores everything and iret's back
 *
 * mingw-w64 i386 PE prepends an underscore to all C symbols, hence the
 * leading "_" everywhere here.
 */

.code32
.section .text

.macro ISR_NOERR num
    .global _isr\num
_isr\num:
    cli
    push    $0
    push    $\num
    jmp     isr_common_stub
.endm

.macro ISR_ERR num
    .global _isr\num
_isr\num:
    cli
    push    $\num
    jmp     isr_common_stub
.endm

.macro IRQ num, vec
    .global _irq\num
_irq\num:
    cli
    push    $0
    push    $\vec
    jmp     irq_common_stub
.endm

/* CPU exceptions 0..31 — the ones with hardware-pushed error codes:
 * 8 (double fault), 10 (invalid TSS), 11 (segment not present),
 * 12 (stack fault), 13 (GP), 14 (page fault), 17 (alignment), 30 (security).
 */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

/* INT 0x80 system-call gate. Reuses the same common stub so the
 * syscall handler sees a normal `struct registers` frame and can
 * read/write EAX (return value) through it. */
.global _isr128
_isr128:
    cli
    push    $0
    push    $128
    jmp     isr_common_stub

/* LAPIC timer (IDT vector 0x40 = 64). Reuses the irq_common_stub
 * but the C handler EOIs the LAPIC instead of the legacy 8259 PIC.
 * Each CPU's LAPIC fires this independently — APs use it as their
 * preemption tick. */
.global _lapic_timer_stub
_lapic_timer_stub:
    cli
    push    $0
    push    $0x40
    jmp     lapic_irq_common_stub

/* Hardware IRQs, remapped to vectors 32..47 */
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

isr_common_stub:
    pusha

    /* Save DS as a clean 32-bit value (zero-extended) */
    xor     %eax, %eax
    mov     %ds,  %ax
    push    %eax

    /* Reload kernel data selector */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs

    /* Pass pointer to register frame (current ESP) */
    push    %esp
    call    _isr_handler
    add     $4, %esp

    /* Restore DS and friends */
    pop     %eax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs

    popa
    add     $8, %esp        /* skip int_no + err_code */
    iret

irq_common_stub:
    pusha

    xor     %eax, %eax
    mov     %ds,  %ax
    push    %eax

    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs

    push    %esp
    call    _irq_handler
    add     $4, %esp

    pop     %eax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs

    popa
    add     $8, %esp
    iret

/* Like irq_common_stub but routes to _lapic_irq_handler — that
 * function sends EOI to the LAPIC (not the PIC) and dispatches to
 * the LAPIC-timer scheduler-tick. Same cpu_local indirection as
 * the regular IRQ path; works on BSP and APs identically. */
lapic_irq_common_stub:
    pusha

    xor     %eax, %eax
    mov     %ds,  %ax
    push    %eax

    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs

    push    %esp
    call    _lapic_irq_handler
    add     $4, %esp

    pop     %eax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs

    popa
    add     $8, %esp
    iret

/*
 * Helper: load IDT register from a struct pointer pushed as the only arg.
 */
.global _idt_flush
_idt_flush:
    mov     4(%esp), %eax
    lidt    (%eax)
    ret
