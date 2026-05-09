#ifndef ADVENTOS_VBE_H
#define ADVENTOS_VBE_H

#include "../include/types.h"

/* Snapshot of the framebuffer state the bootloader negotiated with
 * the BIOS. Filled in by vbe_init() from the summary block at
 * physical 0x9100. If `enabled` is 0, the bootloader's int 10h
 * call failed — kprintf falls back to VGA text mode and fbcon stays
 * dark. */
struct vbe_state {
    int       enabled;
    uint32_t  fb_phys;       /* linear framebuffer physical base */
    uint32_t  fb_size;       /* bytes covered by the FB (pitch*height) */
    uint32_t  width;         /* in pixels                              */
    uint32_t  height;
    uint32_t  pitch;         /* bytes per scanline (≥ width*bpp/8)     */
    uint32_t  bpp;           /* 16, 24, or 32                          */
    volatile uint8_t *fb;    /* identity-mapped virtual ptr to fb_phys */
};

/* TWO-PHASE init.
 *
 *   vbe_capture_bootinfo() — read the bootloader's summary out of low
 *     physical memory (0x9100..0x910C). Must run BEFORE pmm_init,
 *     because the PMM treats that region as free RAM and paging_init
 *     happily allocates page tables on top of our summary. We snapshot
 *     into a static struct so the bytes are safe.
 *
 *   vbe_init()           — validate the snapshot, identity-map the
 *     framebuffer pages into the kernel PD. Must run AFTER paging_init.
 *     Idempotent.
 *
 * Both log via kprintf. If either step fails, ->enabled stays 0 and
 * fbcon_init() will skip painting. */
void              vbe_capture_bootinfo(void);
void              vbe_init(void);

/* Always non-NULL; check ->enabled. Survives across init failure. */
const struct vbe_state *vbe_state(void);

#endif
