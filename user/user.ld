/*
 * Linker script for user-space programs.
 *
 * The kernel ELF loader maps PT_LOAD segments at p_vaddr. We pick a
 * fixed VA of 0x40000000 — same as the in-kernel asm user_program_*
 * placement, so we keep one user-VA convention across the whole tree.
 *
 * .startup must come first in .text so _start (the entry point) ends
 * up at exactly 0x40000000. mkfs.py hard-codes e_entry = 0x40000000.
 *
 * Section names in input objects are PE/COFF (8-char limit): we use
 * .text*, .rdata*, .data*. There is intentionally no .bss output:
 * compile flags (-fno-zero-initialized-in-bss + -fno-common) keep
 * uninitialized globals out of .bss, so filesz == memsz at the loader.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : SUBALIGN(4) {
        /* Session 132 — KEEP() the .startup section so --gc-sections
         * doesn't drop _start.  ENTRY(_start) marks the symbol as a
         * GC root, but only KEEP() guarantees the *section* survives
         * with its required first-in-.text placement (the kernel
         * loader assumes e_entry = 0x40000000). */
        KEEP(*(.startup))         /* _start lives here, must be first */
        *(.text*)
        *(.gnu.linkonce.t.*)
    }

    .rdata : SUBALIGN(4) {
        *(.rdata*)
        *(.rodata*)
        *(.gnu.linkonce.r.*)
    }

    .data : SUBALIGN(4) {
        /* Session 132 — 4-byte anchor that survives --gc-sections.
         * Without it, programs with no live .data input sections
         * (e.g. init, which uses only zero-init BSS globals) end
         * up with the OUTPUT .data section as NOBITS — the folded
         * .bss bytes are no longer PROGBITS-promoted because there
         * was no PROGBITS input to mix them with.  The anchor pads
         * .data start by 4 bytes but keeps the output loadable. */
        LONG(0)

        *(.data*)
        *(.gnu.linkonce.d.*)

        /* Fold .bss INTO .data so zero-initialized statics get real
         * bytes in the .bin output (the linker promotes the input
         * NOBITS sections to PROGBITS as zeros when their output
         * section is PROGBITS). Without this, user.ld's old /DISCARD/
         * for .bss left the symbols resolving to address 0 — every
         * "static int g_x = 0;" would write-fault on first touch. */
        . = ALIGN(4);
        *(.bss*)
        *(COMMON)
        *(.gnu.linkonce.b.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.idata*)
        *(.xdata*)
        *(.pdata*)
        *(.CRT*)
        *(.tls*)
        *(.ctors*)
        *(.dtors*)
        *(.reloc*)
        *(.debug*)
    }
}
