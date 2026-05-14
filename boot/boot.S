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

    /* Load kernel via int 13h LBA extension (ah=42h).  We split the
     * load into THREE 128-sector (64 KiB) reads — QEMU's SeaBIOS
     * accepts 128 per call but rejects 256, so each individual DAP
     * stays ≤ 128.
     *   read 1: LBA   1..128 -> 0x10000  (dap1)
     *   read 2: LBA 129..256 -> 0x20000  (dap2)
     *   read 3: LBA 257..384 -> 0x30000  (dap2 reused, modified)
     * Total: 192 KiB load budget.  DL is preserved by int-13h-ah=42h
     * so we only need `movb drive, %dl` once; AH is *not* preserved
     * (set to status code) so it gets reloaded per call.
     *
     * For the third read we patch dap2 in place (new segment +
     * starting LBA) — saves 16 bytes vs declaring a third DAP entry
     * and keeps the boot sector under 510 bytes. */
    movb    drive, %dl
    mov     $dap1, %si
    mov     $0x42, %ah
    int     $0x13
    jc      disk_err

    mov     $dap2, %si
    mov     $0x42, %ah
    int     $0x13
    jc      disk_err

    movw    $0x3000, dap2+6      /* segment for read 3 */
    movw    $257,    dap2+8      /* LBA low 16 for read 3 (high 48 stays 0) */
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

    /*
     * VBE mode-set. Switch into mode 0x118 (1024x768x24bpp linear FB)
     * via int 0x10 AX=0x4F02. First call AX=0x4F01 to ask the BIOS
     * for the ModeInfoBlock (256 bytes at 0x9000); we copy a few
     * fields out into a kernel-friendly summary at 0x9100:
     *   0x9100  uint32_t  fb_phys
     *   0x9104  uint16_t  width
     *   0x9106  uint16_t  height
     *   0x9108  uint16_t  pitch          (bytes per scanline)
     *   0x910A  uint8_t   bpp
     *   0x910B  uint8_t   ok flag (0=fall back to text mode)
     *
     * Important: int 0x10 (and int 0x15 we used earlier for E820)
     * is allowed to clobber DS/ES per the BIOS-int contract on some
     * real-hardware BIOSes. The IBM-PC convention preserves them
     * but it's not promised. We re-zero ES (and re-set DI) before
     * EACH call, and before any post-call absolute-address read we
     * re-zero DS so the disp16 addressing modes we use here resolve
     * to physical addresses we expect.
     *
     * If anything fails, ok stays 0 and the kernel keeps using VGA
     * text mode at 0xB8000 — no graphics, but the system still boots.
     */
    xor     %ax, %ax
    mov     %ax, %ds                     /* explicit DS = 0 for our writes */
    mov     %ax, %es                     /* ES:DI buffer in segment 0     */
    movb    $0, 0x910B                   /* ok = 0 by default              */

    /* Pre-zero the 256-byte ModeInfoBlock at 0x9000. If the BIOS
     * silently refuses to write the block but returns AX=0x004F
     * (some VGABIOS quirk), we'll still read all-zeros and the
     * kernel's plausibility check will reject it cleanly. */
    push    %di
    mov     $0x9000, %di
    xor     %ax, %ax
    mov     $128, %cx                    /* 128 words = 256 bytes */
    rep stosw
    pop     %di

    /* AX=0x4F01, CX=mode, ES:DI=info-block buffer. */
    mov     $0x9000, %di
    mov     $0x118, %cx
    mov     $0x4F01, %ax
    int     $0x10
    /* Restore segment registers — the BIOS may have touched them. */
    push    %ax
    xor     %ax, %ax
    mov     %ax, %ds
    mov     %ax, %es
    pop     %ax
    cmp     $0x004F, %ax
    jne     vbe_skip

    /* ModeInfoBlock fields (VBE 2.0 spec):
     *   +0x10 w pitch   +0x12 w width   +0x14 w height
     *   +0x19 b bpp     +0x28 d fb_phys */
    mov     0x9010, %ax
    mov     %ax, 0x9108
    mov     0x9012, %ax
    mov     %ax, 0x9104
    mov     0x9014, %ax
    mov     %ax, 0x9106
    mov     0x9019, %al
    mov     %al, 0x910A
    mov     0x9028, %eax
    mov     %eax, 0x9100

    /* Set the mode with linear-FB bit (BX bit 14 = 0x4000). */
    mov     $(0x118 | 0x4000), %bx
    mov     $0x4F02, %ax
    int     $0x10
    push    %ax
    xor     %ax, %ax
    mov     %ax, %ds
    pop     %ax
    cmp     $0x004F, %ax
    jne     vbe_skip
    movb    $1, 0x910B                   /* ok = 1 */
vbe_skip:

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
msg_loaded: .asciz " ok\r\n"
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

/* Disk Address Packets. Total kernel budget: 64 KiB + 128 KiB = 192 KiB.
 * QEMU's SeaBIOS handles a single 256-sector LBA read fine; on real
 * hardware some BIOSes cap at 127, but we don't target real hw. */
dap1:
    .byte 0x10              /* size of packet */
    .byte 0x00              /* reserved */
    .word 128               /* sectors to read (64 KiB) */
    .word 0x0000            /* destination offset */
    .word 0x1000            /* destination segment (linear 0x10000) */
    .quad 1                 /* starting LBA */

dap2:
    .byte 0x10
    .byte 0x00
    .word 128               /* 64 KiB; modified in place for read 3 */
    .word 0x0000
    .word 0x2000            /* destination segment (linear 0x20000) */
    .quad 129               /* starting LBA = 1 + 128 */

/* Pad to 510 bytes, then write boot signature 0x55AA */
.org 510
.word 0xAA55
