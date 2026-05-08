/*
 * Linker script for the 16-bit MBR boot sector.
 *
 * The boot sector is loaded by the BIOS at physical address 0x7C00.
 * boot.S already pads itself to 510 bytes and emits the 0xAA55
 * signature, so this script just lays out a single .text section at
 * 0x7C00 and discards everything else.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x7C00;

    .text : SUBALIGN(1)
    {
        *(.text*)
        *(.data*)
        *(.rdata*)
        *(.rodata*)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.idata*)
        *(.bss*)
        *(COMMON)
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
