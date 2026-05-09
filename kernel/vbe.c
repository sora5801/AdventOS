#include "vbe.h"
#include "paging.h"
#include "kprintf.h"

/* The bootloader stashes a 12-byte summary at physical 0x9100. Layout
 * (matches boot/boot.S):
 *   0x9100  uint32_t  fb_phys
 *   0x9104  uint16_t  width
 *   0x9106  uint16_t  height
 *   0x9108  uint16_t  pitch
 *   0x910A  uint8_t   bpp
 *   0x910B  uint8_t   ok flag (1 = bootloader successfully set the mode)
 *
 * IMPORTANT: this region is in free conventional RAM as far as the PMM
 * is concerned. Once pmm_init runs, paging_init will happily allocate
 * page-table pages on top of it. So we MUST capture the summary into
 * a kernel-private struct (g_boot) before pmm_init touches anything.
 * That capture is done by vbe_capture_bootinfo, called from kmain
 * very early. The later vbe_init reads from g_boot, not from physical
 * 0x9100. */
#define VBE_SUMMARY_PHYS  0x9100u

static struct {
    int       captured;
    uint32_t  fb_phys;
    uint16_t  width;
    uint16_t  height;
    uint16_t  pitch;
    uint8_t   bpp;
    uint8_t   ok;
} g_boot;

static struct vbe_state g_state;

void vbe_capture_bootinfo(void) {
    if (g_boot.captured) return;
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)VBE_SUMMARY_PHYS;
    g_boot.fb_phys = *(volatile uint32_t *)(p + 0x00);
    g_boot.width   = *(volatile uint16_t *)(p + 0x04);
    g_boot.height  = *(volatile uint16_t *)(p + 0x06);
    g_boot.pitch   = *(volatile uint16_t *)(p + 0x08);
    g_boot.bpp     = *(volatile uint8_t  *)(p + 0x0A);
    g_boot.ok      = *(volatile uint8_t  *)(p + 0x0B);
    g_boot.captured = 1;
}

void vbe_init(void) {
    if (g_state.enabled) return;
    if (!g_boot.captured) {
        kprintf("vbe: vbe_init called before vbe_capture_bootinfo\n");
        return;
    }
    if (!g_boot.ok) {
        kprintf("vbe: bootloader couldn't set graphics mode "
                "(int 10h failed) — staying in VGA text mode\n");
        return;
    }

    /* Sanity-check the dimensions. A bogus mode info block from a weird
     * BIOS could land us here with absurd values; bail rather than
     * mapping gigabytes of garbage. */
    if (g_boot.width == 0 || g_boot.height == 0 || g_boot.pitch == 0 ||
        g_boot.width > 8192 || g_boot.height > 8192 ||
        (g_boot.bpp != 16 && g_boot.bpp != 24 && g_boot.bpp != 32)) {
        kprintf("vbe: implausible mode info "
                "(%ux%u %u-bpp pitch=%u) — disabled\n",
                (unsigned)g_boot.width, (unsigned)g_boot.height,
                (unsigned)g_boot.bpp, (unsigned)g_boot.pitch);
        return;
    }


    uint32_t fb_size = (uint32_t)g_boot.pitch * (uint32_t)g_boot.height;

    /* Identity-map every page of the FB into the kernel PD. The FB on
     * QEMU std-vga lives at a high physical address (e.g. 0xFD000000)
     * — well outside the RAM range that paging_init covered. PCD/PWT
     * are left clear; the chipset's MTRR config decodes write-combining
     * for the FB region on most boxes. We could be stricter here but
     * QEMU treats the framebuffer as plain RAM-like memory and a few
     * stale CPU cache lines never matter for a console. */
    uintptr_t va_start = g_boot.fb_phys & ~0xFFFu;
    uintptr_t va_end   = (g_boot.fb_phys + fb_size + 0xFFFu) & ~0xFFFu;
    for (uintptr_t a = va_start; a < va_end; a += 0x1000) {
        if (paging_map(a, a, PTE_PRESENT | PTE_WRITABLE) < 0) {
            kprintf("vbe: paging_map failed at 0x%x — disabled\n",
                    (unsigned)a);
            return;
        }
    }

    g_state.enabled = 1;
    g_state.fb_phys = g_boot.fb_phys;
    g_state.fb_size = fb_size;
    g_state.width   = g_boot.width;
    g_state.height  = g_boot.height;
    g_state.pitch   = g_boot.pitch;
    g_state.bpp     = g_boot.bpp;
    g_state.fb      = (volatile uint8_t *)(uintptr_t)g_boot.fb_phys;

    kprintf("vbe: %ux%u %u-bpp linear FB at 0x%x (%u KiB mapped)\n",
            (unsigned)g_boot.width, (unsigned)g_boot.height, (unsigned)g_boot.bpp,
            (unsigned)g_boot.fb_phys, (unsigned)(fb_size >> 10));
}

const struct vbe_state *vbe_state(void) {
    return &g_state;
}
