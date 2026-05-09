/*
 * AP boot trampoline. Lives in section .text.aptramp; the BSP
 * memcpys the bytes between _ap_trampoline_start and
 * _ap_trampoline_end down to physical address 0x8000 at runtime
 * and then sends each AP an INIT-SIPI-SIPI pointing at that page.
 *
 * The AP wakes in 16-bit real mode at CS:IP = 0800:0000. Switches
 * to 32-bit pmode using a tiny inline GDT, loads the kernel CR3,
 * enables paging, and call's ap_entry() with its logical id.
 *
 * Layout is hand-rolled so the assembler doesn't need to compute
 * label differences across .code16/.code32 boundaries — PE/COFF's
 * gas chokes on those. Each constant is an absolute offset from
 * AP_BOOT_PHYS / start of trampoline, baked in at assembly time.
 *   0x000  16-bit real-mode entry
 *   0x040  GDT pointer (lgdt operand) and 16-bit GDT
 *   0x060  32-bit protected-mode entry
 *   0x100  parameter cells: ap_cr3, ap_counter, ap_entry_addr,
 *          ap_stacks[8]
 */

#define AP_BOOT_PHYS  0x8000

.code16
.section .text.aptramp, "ax"
.global _ap_trampoline_start
.global _ap_trampoline_end
.global _ap_cr3
.global _ap_counter
.global _ap_stacks
.global _ap_entry_addr

_ap_trampoline_start:
    cli
    cld

    /* Stamp 0xCAFE into physical 0x8200 as a "we got the SIPI"
     * marker the BSP can poll. Useful diagnostic if things go
     * wrong further along. */
    mov     %cs, %ax
    mov     %ax, %ds
    movw    $0xCAFE, 0x200

    /* CS = 0x800 (the SIPI segment). Mirror to es/ss too. */
    mov     %ax, %es
    mov     %ax, %ss
    mov     $0x1000, %sp        /* tiny temporary real-mode stack    */

    /* Load the GDT from cs:0x40. We hand-encode the lgdt: gas's
     * 16-bit `lgdtl 0x40` resolves to an SI-relative addressing
     * mode, but %si comes up undefined after the SIPI handshake.
     * The hand-encoded form uses direct disp16 addressing — no
     * register dependency. The bytes are `66 2E 0F 01 16 40 00`
     * = 0x66 (operand-size 32) + 0x2E (cs override) + 0x0F 01
     * with /2 ModR/M (LGDT) + 0x16 (mod=00, rm=110, direct disp16)
     * + disp16 = 0x0040. */
.byte 0x66, 0x2E, 0x0F, 0x01, 0x16, 0x40, 0x00    /* lgdtl cs:0x40 */

    /* Enable PE. */
    mov     %cr0, %eax
    or      $1, %eax
    mov     %eax, %cr0

    /* Far jump to flush the prefetch queue and reload CS. The
     * destination is the absolute physical address AP_BOOT_PHYS+0x60
     * (the .org-aligned pmode entry below). The 0x66 prefix in
     * 16-bit code makes this a ptr16:32 form (= 4-byte offset +
     * 2-byte selector); we hand-encode because gas's `ljmpl` in
     * a .code16 section produced encodings the assembler later
     * mis-decoded. Bytes: 66 EA off32 sel16. */
.byte 0x66, 0xEA, 0x60, 0x80, 0x00, 0x00, 0x08, 0x00   /* ljmpl 0x08, 0x8060 */

/* ---- inline gdtr at offset 0x40, GDT at offset 0x48 ---- */
.org 0x40
    .word 0x0017                 /* gdtr.limit  = 24-1 (3 entries × 8) */
    .long AP_BOOT_PHYS + 0x48    /* gdtr.base   = abs phys of GDT */
    /* gdtr is 6 bytes; pad 2 bytes so the GDT itself sits at 0x48
     * (the base we just promised the lgdt operand). */
    .word 0
.org 0x48
    .quad 0x0000000000000000     /* null */
    .quad 0x00CF9A000000FFFF     /* 0x08: code32, base=0, limit=4G, ring 0 */
    .quad 0x00CF92000000FFFF     /* 0x10: data32, base=0, limit=4G, ring 0 */

/* ---- 32-bit protected-mode entry, fixed at offset 0x60 ---- */
.code32
.org 0x60
    /* CS is now flat (base=0) but ds/es/ss still have their real-
     * mode cached descriptors (base = old_seg * 16). Reload data
     * segments FIRST so subsequent absolute addresses go to the
     * linear address we wrote, not a real-mode-shifted version. */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Load the kernel's master CR3 from the param cell at 0x100. */
    mov     (AP_BOOT_PHYS + 0x100), %eax
    mov     %eax, %cr3

    /* Enable paging (CR0.PG = 1). After this the kernel's identity
     * map covers low memory + the kernel image; any kernel-side
     * branch is reachable. */
    mov     %cr0, %eax
    or      $0x80000000, %eax
    mov     %eax, %cr0

    /* Atomically grab our logical CPU id from the shared counter at
     * 0x104. lock xadd does the read-modify-write with bus locking;
     * eax ends up with the OLD counter value (= our id). */
    mov     $1, %eax
    lock xaddl %eax, (AP_BOOT_PHYS + 0x104)

    /* Pull our pre-allocated kernel stack pointer from the stack
     * table at 0x10C (table base + id*4). */
    mov     (AP_BOOT_PHYS + 0x10C)(, %eax, 4), %esp

    /* Push id as the argument to ap_entry. */
    pushl   %eax

    /* Jump to ap_entry. The entry-address cell at 0x108 holds the
     * kernel-resolved virtual address of ap_entry. */
    mov     (AP_BOOT_PHYS + 0x108), %eax
    call    *%eax

    /* Should never return; if it does, halt. */
1:  cli
    hlt
    jmp     1b

/* ---- parameter cells, fixed at offset 0x100 ---- */
.org 0x100
_ap_cr3:        .long 0          /* kernel master CR3                  */
_ap_counter:    .long 0          /* lock xadd starting count           */
_ap_entry_addr: .long 0          /* address of ap_entry() in kernel    */
_ap_stacks:     .long 0,0,0,0,0,0,0,0    /* 8 slots, kernel stack tops */

.org 0x130
_ap_trampoline_end:
