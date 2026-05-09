/*
 * AdventOS — 16-bit MBR bootloader
 *
 * The BIOS loads us at 0000:7C00 with DL = boot drive.
 * We:
 *   1. Set up real-mode segments and stack
 *   2. Load N sectors of kernel from LBA 1 to physical 0x10000 via int 13h ah=42h
 *   3. Enable A20, install a minimal flat GDT
 *   4. Switch to 32-bit protected mode
 *   5. Far jump to the kernel entry at 0x10000
 */

.code16
.section .text
.global _start

_start:
    cli
    xor     %ax, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss
    mov     $0x7C00, %sp
    sti

    /* Save BIOS-supplied boot drive number */
    movb    %dl, drive

    /* Print boot message via BIOS teletype */
    mov     $msg_boot, %si
    call    print

    /* Load kernel via int 13h LBA extension (ah=42h) */
    movb    drive, %dl
    mov     $dap, %si
    mov     $0x42, %ah
    int     $0x13
    jc      disk_err

    mov     $msg_loaded, %si
    call    print

    /*
     * Query the BIOS E820 memory map.
     * Layout the kernel will see at linear 0x8000:
     *   0x8000  uint32_t  count
     *   0x8004  entries[count], 24 bytes each
     */
    xor     %ax, %ax
    mov     %ax, %es                     /* ES = 0 */
    mov     $0x8004, %di                 /* ES:DI = entry buffer */
    xor     %ebx, %ebx                   /* continuation = 0 */
    xor     %ebp, %ebp                   /* entry counter */
e820_loop:
    movl    $1, %es:20(%di)              /* ACPI 3.0 attrs: valid */
    mov     $24, %ecx
    mov     $0x534D4150, %edx            /* 'SMAP' */
    mov     $0xE820, %eax
    int     $0x15
    jc      e820_done                    /* CF on first call = unsupported */
    cmp     $0x534D4150, %eax
    jne     e820_done

    /* Skip zero-length entries (BIOS quirk on some boxes) */
    movl    %es:8(%di), %eax
    or      %es:12(%di), %eax
    jz      e820_skip

    inc     %ebp
    add     $24, %di
e820_skip:
    test    %ebx, %ebx                   /* EBX = 0 means last entry */
    jz      e820_done
    cmp     $50, %ebp
    jl      e820_loop
e820_done:
    movl    %ebp, 0x8000                 /* store final count */

    /* Disable interrupts and prepare for protected mode */
    cli

    /* Enable A20 via fast gate (port 0x92, bit 1) */
    in      $0x92, %al
    test    $2, %al
    jnz     a20_done
    or      $2, %al
    and     $0xFE, %al        /* Clear bit 0 to avoid fast reset */
    out     %al, $0x92
a20_done:

    /* Load GDT */
    lgdt    gdtr

    /* Set CR0.PE=1 to enter protected mode */
    mov     %cr0, %eax
    or      $1, %eax
    mov     %eax, %cr0

    /* Far jump to flush prefetch and reload CS as 32-bit */
    ljmp    $0x08, $pm_entry

disk_err:
    mov     $msg_err, %si
    call    print
hang:
    hlt
    jmp     hang

/*
 * print: print a NUL-terminated string at DS:SI via BIOS int 10h ah=0Eh.
 * Clobbers AX, BX.
 */
print:
    push    %ax
    push    %bx
print_loop:
    lodsb
    test    %al, %al
    jz      print_done
    mov     $0x0E, %ah
    mov     $0x0007, %bx
    int     $0x10
    jmp     print_loop
print_done:
    pop     %bx
    pop     %ax
    ret

.code32
pm_entry:
    /* Reload data segments with kernel data selector (0x10) */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss
    mov     $0x90000, %esp

    /* Pass boot drive in EBX so the kernel can recover it later */
    movzbl  drive, %ebx

    /* Far jump to kernel entry at physical 0x10000 */
    ljmp    $0x08, $0x10000

/* ------------------------------------------------------------------ data */

drive:      .byte 0
msg_boot:   .asciz "AdventOS bootloader v0.1\r\nLoading kernel..."
msg_loaded: .asciz " ok\r\nEntering protected mode\r\n"
msg_err:    .asciz "\r\nFATAL: disk read failed\r\n"

.align 8
gdt:
    /* Null descriptor */
    .quad 0x0000000000000000
    /* 0x08: 32-bit code, base=0, limit=4GB, ring 0, exec/read */
    .word 0xFFFF, 0x0000
    .byte 0x00, 0x9A, 0xCF, 0x00
    /* 0x10: 32-bit data, base=0, limit=4GB, ring 0, read/write */
    .word 0xFFFF, 0x0000
    .byte 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdtr:
    .word gdt_end - gdt - 1
    .long gdt

/* Disk Address Packet for int 13h ah=42h */
dap:
    .byte 0x10              /* size of packet */
    .byte 0x00              /* reserved */
    .word 128               /* number of sectors to read (64 KiB) — was 112; bumped in session 21 when UDP/DHCP/DNS pushed kernel.bin past 56 KiB */
    .word 0x0000            /* destination offset */
    .word 0x1000            /* destination segment (linear 0x10000) */
    .quad 1                 /* starting LBA */

/* Pad to 510 bytes, then write boot signature 0x55AA */
.org 510
.word 0xAA55
