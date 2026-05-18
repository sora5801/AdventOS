#ifndef ADVENTOS_LIBGFX_H
#define ADVENTOS_LIBGFX_H

/*
 * libgfx — software drawing library for AdventOS userspace.
 *
 * Built on session 107's framebuffer syscalls. A graphical program
 * calls gfx_init() once, then uses the drawing primitives. The
 * library packs RGB into the right pixel format for the negotiated
 * bpp; the user passes 0xRRGGBB and libgfx handles the rest.
 *
 *   struct gfx_ctx ctx;
 *   if (gfx_init(&ctx, 0x50000000u) < 0) ...;
 *   gfx_clear(&ctx, 0x202020);
 *   gfx_fill_rect(&ctx, 100, 100, 200, 150, 0xE03030);
 *   gfx_line(&ctx, 0, 0, ctx.width-1, ctx.height-1, 0xFFFFFF);
 *   gfx_text(&ctx, 16, 16, "hello, world", 0xFFFFFF, 0x202020);
 *   gfx_release(&ctx);
 *
 * Sessions ahead in Path C will add a backbuffer (110), mouse-aware
 * dirty regions, and a window-manager facade. The drawing primitives
 * stay the same.
 */

#include "../include/types.h"

struct gfx_ctx {
    /* All drawing primitives write to ->fb. In single-buffered mode
     * (gfx_init) that's the mapped framebuffer itself; in double-
     * buffered mode (gfx_init_db) it's a malloc'd backbuffer and the
     * real fb is reachable via ->fb_real. gfx_present() blits the
     * backbuffer to ->fb_real (no-op in single-buffered mode). */
    volatile unsigned char *fb;       /* drawing target */
    volatile unsigned char *fb_real;  /* the mapped framebuffer */
    unsigned char          *back;     /* malloc'd backbuffer (NULL = single-buf) */
    unsigned int  user_va;            /* where the FB is mapped */
    unsigned int  width;
    unsigned int  height;
    unsigned int  pitch;              /* bytes per scanline */
    unsigned int  bpp;                /* 16, 24, or 32 */
    unsigned int  fb_size;            /* pitch * height */
};

/* Common 24-bit color names. Match kernel/fbcon.h's FBC_* constants. */
#define GFX_BLACK        0x000000u
#define GFX_GREY         0xC0C0C0u
#define GFX_WHITE        0xFFFFFFu
#define GFX_RED          0xE03030u
#define GFX_GREEN        0x30E030u
#define GFX_BLUE         0x4080E0u
#define GFX_YELLOW       0xE0E030u
#define GFX_CYAN         0x30E0E0u
#define GFX_MAGENTA      0xE030E0u
#define GFX_DARK_GREY    0x202020u

/* Lifecycle: ask kernel for FB geometry, map it into our PD at
 * `user_va`, fill in the ctx. Returns 0 on success, -1 if either
 * the FB is unavailable or another task already owns it. */
int  gfx_init    (struct gfx_ctx *ctx, unsigned int user_va);

/* Session 110 — double-buffered init. Allocates a malloc'd back-
 * buffer of fb_size bytes; all primitives draw to it. Call
 * gfx_present() to push it to the screen. Use this for animation
 * to avoid tearing. Returns 0 / -1. */
int  gfx_init_db (struct gfx_ctx *ctx, unsigned int user_va);

/* Copy the backbuffer to the real framebuffer. No-op for single-
 * buffered contexts. Constant time (one memcpy of fb_size bytes). */
void gfx_present (struct gfx_ctx *ctx);

/* Release the FB so fbcon (or another task) can take over.
 * Auto-called at process exit by the kernel as well. Also frees
 * the backbuffer if double-buffered. */
void gfx_release (struct gfx_ctx *ctx);

/* Pack a 0xRRGGBB into the kernel's pixel format for this ctx's bpp.
 * Cached pack of the most common colors at gfx_init time would be
 * a micro-opt; not done yet. */
unsigned int gfx_pack (const struct gfx_ctx *ctx, unsigned int rgb);

/* Drawing primitives — all clip to the framebuffer dimensions. */
void gfx_put_pixel (struct gfx_ctx *ctx, int x, int y, unsigned int rgb);
void gfx_clear     (struct gfx_ctx *ctx, unsigned int rgb);
void gfx_fill_rect (struct gfx_ctx *ctx, int x, int y, int w, int h, unsigned int rgb);
/* Single-pixel-thick stroke. Bresenham. */
void gfx_line      (struct gfx_ctx *ctx, int x0, int y0, int x1, int y1, unsigned int rgb);
/* 1-pixel-thick rectangle outline. */
void gfx_rect      (struct gfx_ctx *ctx, int x, int y, int w, int h, unsigned int rgb);

/* 8x8 monospace font. `c` is ASCII 0x20..0x7F; other chars render as
 * '?'. Top-left of the cell is at (x, y). Pass GFX_TRANSPARENT as bg
 * to skip the background-fill pass. */
#define GFX_TRANSPARENT  0xFFFFFFFFu
void gfx_glyph     (struct gfx_ctx *ctx, int x, int y, char c, unsigned int fg, unsigned int bg);
/* Render NUL-terminated `s` from (x, y); advances 8 px per char.
 * Doesn't wrap. */
void gfx_text      (struct gfx_ctx *ctx, int x, int y, const char *s,
                    unsigned int fg, unsigned int bg);

#endif
