.code32
.section .text

.global _gdt_flush
_gdt_flush:
    mov     4(%esp), %eax       /* address of gdtr */
    lgdt    (%eax)
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss
    ljmp    $0x08, $.flush
.flush:
    ret
