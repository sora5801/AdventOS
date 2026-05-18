/*
 * gfx.c — session 107 / Path C phase 1 smoke test for userspace
 * framebuffer access.
 *
 *   gfx [seconds]    paint a test card; sleep `seconds` (default 4);
 *                    release the FB and let fbcon resume.
 *
 * Shows:
 *   - top-left RGB gradient block (proves per-pixel write works)
 *   - color bars across the top stripe
 *   - centered filled rectangle
 *   - a thin cross through the screen center
 *
 * Coexists with fbcon: SYS_FB_MAP mutes fbcon while we paint; on
 * SYS_FB_UNMAP (or task exit) fbcon repaints itself from its
 * scrollback buffer the next time something writes to it.
 *
 * Pixel format the kernel negotiated is reported via SYS_FB_INFO.
 * Most QEMU defaults give us 32 bpp with 0x00BBGGRR layout per dword.
 */

#include "libuser.h"

#define FB_VA  0x50000000u   /* somewhere clear of stack/heap/code */

/* Pack a 0xRRGGBB into the kernel-side format for the given bpp.
 * Mirrors kernel/fbcon.c::pack_pixel. */
static unsigned int pack(unsigned int rgb, unsigned int bpp) {
    unsigned int r = (rgb >> 16) & 0xFF;
    unsigned int g = (rgb >>  8) & 0xFF;
    unsigned int b =  rgb        & 0xFF;
    if (bpp == 16) {
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    /* 24 and 32 share BBGGRR byte order in memory. */
    return (r << 16) | (g << 8) | b;
}

/* Plot one pixel — caller must clip. */
static void plot(volatile unsigned char *fb,
                 unsigned int pitch, unsigned int bpp,
                 unsigned int x, unsigned int y,
                 unsigned int packed) {
    volatile unsigned char *row = fb + (unsigned int)y * pitch;
    if (bpp == 32) {
        ((volatile unsigned int *)row)[x] = packed;
    } else if (bpp == 24) {
        volatile unsigned char *p = row + x * 3;
        p[0] = (unsigned char)(packed);
        p[1] = (unsigned char)(packed >> 8);
        p[2] = (unsigned char)(packed >> 16);
    } else {
        ((volatile unsigned short *)row)[x] = (unsigned short)packed;
    }
}

static void fill_rect(volatile unsigned char *fb,
                      unsigned int pitch, unsigned int bpp,
                      unsigned int x, unsigned int y,
                      unsigned int w, unsigned int h,
                      unsigned int rgb) {
    unsigned int p = pack(rgb, bpp);
    for (unsigned int yy = 0; yy < h; yy++) {
        for (unsigned int xx = 0; xx < w; xx++) {
            plot(fb, pitch, bpp, x + xx, y + yy, p);
        }
    }
}

int main(int argc, char **argv) {
    int seconds = 4;
    if (argc >= 2) seconds = atoi(argv[1]);
    if (seconds <= 0) seconds = 4;

    struct sys_fb_info info;
    if (sys_fb_info(&info) < 0 || !info.enabled) {
        printf("gfx: no VBE framebuffer — boot QEMU with VBE enabled\n");
        return 1;
    }
    printf("gfx: %ux%u %ubpp, pitch=%u, %u KiB\n",
           info.width, info.height, info.bpp, info.pitch, info.fb_size >> 10);

    if (sys_fb_map(FB_VA) < 0) {
        printf("gfx: SYS_FB_MAP failed (already owned?)\n");
        return 1;
    }

    volatile unsigned char *fb = (volatile unsigned char *)FB_VA;
    unsigned int w     = info.width;
    unsigned int h     = info.height;
    unsigned int pitch = info.pitch;
    unsigned int bpp   = info.bpp;

    /* Wipe to dark gray. */
    fill_rect(fb, pitch, bpp, 0, 0, w, h, 0x202020);

    /* Color bars across the top 64 px. */
    unsigned int colors[8] = {
        0xFF0000, 0xFF7F00, 0xFFFF00, 0x00FF00,
        0x00FFFF, 0x0000FF, 0x8000FF, 0xFFFFFF,
    };
    unsigned int bar_w = w / 8;
    for (int i = 0; i < 8; i++) {
        fill_rect(fb, pitch, bpp,
                  (unsigned int)i * bar_w, 0,
                  bar_w, 64, colors[i]);
    }

    /* RGB gradient block in the top-left (below the bars). */
    unsigned int gx0 = 32, gy0 = 96, gw = 256, gh = 256;
    if (gx0 + gw <= w && gy0 + gh <= h) {
        for (unsigned int yy = 0; yy < gh; yy++) {
            for (unsigned int xx = 0; xx < gw; xx++) {
                unsigned int r = xx;
                unsigned int g = yy;
                unsigned int b = (xx + yy) >> 1;
                unsigned int rgb = (r << 16) | (g << 8) | b;
                plot(fb, pitch, bpp, gx0 + xx, gy0 + yy, pack(rgb, bpp));
            }
        }
    }

    /* Centered filled rectangle. */
    unsigned int rw = 200, rh = 120;
    unsigned int rx = (w > rw) ? ((w - rw) / 2) : 0;
    unsigned int ry = (h > rh) ? ((h - rh) / 2) : 0;
    fill_rect(fb, pitch, bpp, rx, ry, rw, rh, 0x40A0F0);

    /* A thin cross through the screen center (1px lines). */
    unsigned int cx = w / 2, cy = h / 2;
    unsigned int white = pack(0xFFFFFF, bpp);
    for (unsigned int x = 0; x < w; x++) plot(fb, pitch, bpp, x, cy, white);
    for (unsigned int y = 0; y < h; y++) plot(fb, pitch, bpp, cx, y, white);

    /* Hold for `seconds` so the human can look at it. */
    for (int s = 0; s < seconds; s++) sys_sleep_ms(1000);

    sys_fb_unmap();
    /* fbcon resumes; its next write repaints the console. */
    printf("gfx: released\n");
    return 0;
}
