/*
 * Linker script for the 32-bit kernel.
 *
 * The bootloader loads sectors 1..N from the disk image to physical
 * 0x10000 and far-jumps to that address. So .text.entry MUST be the
 * first thing in the binary.
 */

ENTRY(_kernel_entry)

SECTIONS
{
    . = 0x10000;

    _kernel_start = .;

    .text : SUBALIGN(4)
    {
        KEEP(*(.text.entry))
        *(.text*)
        *(.gnu.linkonce.t.*)
        /* AP trampoline. Position-independent inside the section;
         * the BSP memcpys it to physical 0x8000 at runtime. The
         * symbols ap_trampoline_start/_end are defined in the
         * trampoline source. */
        KEEP(*(.text.aptramp))
    }

    .rdata : SUBALIGN(4)
    {
        *(.rdata*)
        *(.rodata*)
        *(.gnu.linkonce.r.*)
    }

    .data : SUBALIGN(4)
    {
        *(.data*)
        *(.gnu.linkonce.d.*)
    }

    /* Bytes of the ring-3 programs we ship with the kernel. Each gets
     * its own short-named section so the kernel can find and copy them
     * out independently. Section names kept <= 8 chars (PE/COFF). */
    . = ALIGN(0x1000);
    .up1 : SUBALIGN(4)
    {
        _up1_start = .;
        *(.up1*)
        _up1_end = .;
    }
    . = ALIGN(0x1000);
    .up2 : SUBALIGN(4)
    {
        _up2_start = .;
        *(.up2*)
        _up2_end = .;
    }

    . = ALIGN(16);
    _bss_start = .;
    .bss : SUBALIGN(4)
    {
        *(.bss*)
        *(COMMON)
        *(.gnu.linkonce.b.*)
    }
    _bss_end = .;

    _kernel_end = .;

    /DISCARD/ :
    {
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
