/*
 * Linker script for libc.bin.
 *
 * libc.bin is a flat binary loaded by the kernel at virtual address
 * 0x70000000 in every user process. We link it at that address so
 * absolute calls (call rel32 → indirect into .text) and global-data
 * references all resolve correctly without any runtime relocation.
 *
 * Layout requirements:
 *   - .exports.header at +0x000  (16 bytes: struct libc_header)
 *   - .exports.table  at +0x010  (256 bytes for 64 ptrs)
 *   - .text           at +0x400  (1 KiB padding for future export
 *                                 additions without breaking ABI)
 *   - .rodata, .data, .bss after
 *
 * Section names use PE/COFF (8-char limit) — the GAS i386pe target
 * is what mingw32 produces; .exports.header / .exports.table are
 * over the limit so we collapse the wildcard match.
 */

ENTRY(libc_info)

SECTIONS
{
    . = 0x70000000;

    /* Header + export table — placed first so user programs can
     * reach LIBC_BASE[index] without having to know the .text size. */
    .exports : SUBALIGN(4) {
        KEEP(*(.exports.header))
        KEEP(*(.exports.table))
        . = 0x400;          /* pad to 1 KiB before .text */
    }

    .text : SUBALIGN(16) {
        *(.text*)
        *(.gnu.linkonce.t.*)
    }

    .rdata : SUBALIGN(4) {
        *(.rdata*)
        *(.rodata*)
        *(.gnu.linkonce.r.*)
    }

    .data : SUBALIGN(4) {
        *(.data*)
        *(.gnu.linkonce.d.*)

        /* Fold .bss into .data, same trick as user.ld — keeps zero-
         * initialized statics from resolving to address 0 when bss is
         * stripped from the flat binary. */
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
