/*
 * 32-bit kernel entry stub. Loaded at physical 0x10000.
 *
 * Bootloader has already:
 *   - set up a flat GDT (CS=0x08, DS=0x10)
 *   - cleared interrupts
 *   - set ESP = 0x90000
 * We:
 *   - install our own stack
 *   - clear .bss
 *   - call kmain
 *   - if it returns, halt forever
 */

.code32
.section .text.entry, "ax"
.global _kernel_entry

_kernel_entry:
    cli

    /* Reload data segments to be safe */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Switch to our own stack */
    mov     $stack_top, %esp
    mov     %esp, %ebp

    /* Zero .bss */
    cld
    xor     %eax, %eax
    mov     $_bss_start, %edi
    mov     $_bss_end, %ecx
    sub     %edi, %ecx
    jz      .bss_zeroed
    rep     stosb
.bss_zeroed:

    /* Push 0 as fake return address so backtraces terminate */
    push    $0
    push    $0

    /* boot drive number was passed in EBX from the bootloader */
    push    %ebx
    call    _kmain
    add     $4, %esp

.halt:
    cli
    hlt
    jmp     .halt

.bss
.align 16
.global stack_bottom
stack_bottom:
    .skip 32768
.global stack_top
stack_top:
