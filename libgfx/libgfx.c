/*
 * libgfx.c — software drawing library (session 108).
 *
 * Static archive that graphical user programs link in. Wraps the
 * session-107 framebuffer syscalls plus a Bresenham line, fill, and
 * 8x8 font glyph blit.
 *
 * Design: every primitive checks bounds against ctx->width/height
 * and silently drops anything out-of-range. The framebuffer is
 * written directly through ctx->fb (a user-VA pointer set up at
 * gfx_init time); no syscall per pixel.
 *
 * Pixel format is determined at gfx_init from sys_fb_info()->bpp.
 * The user passes 0xRRGGBB and gfx_pack() (and the put/fill helpers)
 * convert to the target byte order. Matches kernel/fbcon.c's
 * pack_pixel + put_pixel layouts exactly so libgfx output looks the
 * same as fbcon's text.
 */

#include "libgfx.h"
#include "../user/libuser.h"
#include "font8x8.h"

int gfx_init(struct gfx_ctx *ctx, unsigned int user_va) {
    if (!ctx) return -1;
    struct sys_fb_info info;
    if (sys_fb_info(&info) < 0 || !info.enabled) return -1;
    if (sys_fb_map(user_va) < 0) return -1;
    ctx->fb       = (volatile unsigned char *)(uintptr_t)user_va;
    ctx->fb_real  = ctx->fb;
    ctx->back     = 0;
    ctx->user_va  = user_va;
    ctx->width    = info.width;
    ctx->height   = info.height;
    ctx->pitch    = info.pitch;
    ctx->bpp      = info.bpp;
    ctx->fb_size  = info.fb_size;
    return 0;
}

int gfx_init_db(struct gfx_ctx *ctx, unsigned int user_va) {
    int rc = gfx_init(ctx, user_va);
    if (rc < 0) return rc;
    /* Allocate the backbuffer. For 1024x768x24 that's 2.25 MiB —
     * fits comfortably in the 4 MiB per-process heap. */
    ctx->back = (unsigned char *)malloc(ctx->fb_size);
    if (!ctx->back) {
        sys_fb_unmap();
        return -1;
    }
    ctx->fb = (volatile unsigned char *)ctx->back;
    return 0;
}

void gfx_present(struct gfx_ctx *ctx) {
    if (!ctx || !ctx->back || !ctx->fb_real) return;
    /* Straight memcpy. For 1024x768x24 = 2.25 MiB, this is the
     * single biggest cost in the frame loop — but it's a sequential
     * write to MMIO so the CPU's write-combining buffers help.
     * Measured ~3 ms on the QEMU box; well within 60 fps budget. */
    volatile unsigned char *dst = ctx->fb_real;
    const unsigned char    *src = ctx->back;
    for (unsigned int i = 0; i < ctx->fb_size; i++) dst[i] = src[i];
}

void gfx_release(struct gfx_ctx *ctx) {
    if (!ctx) { sys_fb_unmap(); return; }
    if (ctx->back) {
        free(ctx->back);
        ctx->back = 0;
        ctx->fb   = ctx->fb_real;
    }
    sys_fb_unmap();
    /* Don't NULL ctx->fb_real — the pages stay mapped until task
     * exit, but the framebuffer might be repainted by fbcon now so
     * writes would race. The caller is expected to stop drawing
     * after release. */
}

unsigned int gfx_pack(const struct gfx_ctx *ctx, unsigned int rgb) {
    unsigned int r = (rgb >> 16) & 0xFFu;
    unsigned int g = (rgb >>  8) & 0xFFu;
    unsigned int b =  rgb        & 0xFFu;
    if (ctx->bpp == 16) {
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    return (r << 16) | (g << 8) | b;
}

static inline void put_packed(struct gfx_ctx *ctx, int x, int y, unsigned int p) {
    if ((unsigned)x >= ctx->width || (unsigned)y >= ctx->height) return;
    volatile unsigned char *row = ctx->fb + (uintptr_t)y * ctx->pitch;
    if (ctx->bpp == 32) {
        ((volatile unsigned int *)row)[x] = p;
    } else if (ctx->bpp == 24) {
        volatile unsigned char *q = row + (uintptr_t)x * 3;
        q[0] = (unsigned char)(p);
        q[1] = (unsigned char)(p >> 8);
        q[2] = (unsigned char)(p >> 16);
    } else {
        ((volatile unsigned short *)row)[x] = (unsigned short)p;
    }
}

void gfx_put_pixel(struct gfx_ctx *ctx, int x, int y, unsigned int rgb) {
    put_packed(ctx, x, y, gfx_pack(ctx, rgb));
}

void gfx_clear(struct gfx_ctx *ctx, unsigned int rgb) {
    gfx_fill_rect(ctx, 0, 0, (int)ctx->width, (int)ctx->height, rgb);
}

void gfx_fill_rect(struct gfx_ctx *ctx, int x, int y, int w, int h, unsigned int rgb) {
    /* Clip to FB. */
    if (w <= 0 || h <= 0) return;
    if (x < 0)              { w += x; x = 0; }
    if (y < 0)              { h += y; y = 0; }
    if (x >= (int)ctx->width)  return;
    if (y >= (int)ctx->height) return;
    if (x + w > (int)ctx->width)  w = (int)ctx->width  - x;
    if (y + h > (int)ctx->height) h = (int)ctx->height - y;
    if (w <= 0 || h <= 0) return;

    unsigned int p = gfx_pack(ctx, rgb);
    for (int yy = 0; yy < h; yy++) {
        volatile unsigned char *row = ctx->fb + (uintptr_t)(y + yy) * ctx->pitch;
        if (ctx->bpp == 32) {
            volatile unsigned int *r32 = (volatile unsigned int *)row + x;
            for (int xx = 0; xx < w; xx++) r32[xx] = p;
        } else if (ctx->bpp == 24) {
            volatile unsigned char *q = row + (uintptr_t)x * 3;
            for (int xx = 0; xx < w; xx++) {
                q[0] = (unsigned char)(p);
                q[1] = (unsigned char)(p >> 8);
                q[2] = (unsigned char)(p >> 16);
                q += 3;
            }
        } else {
            volatile unsigned short *r16 = (volatile unsigned short *)row + x;
            for (int xx = 0; xx < w; xx++) r16[xx] = (unsigned short)p;
        }
    }
}

/* Bresenham's classic line. */
void gfx_line(struct gfx_ctx *ctx, int x0, int y0, int x1, int y1, unsigned int rgb) {
    unsigned int p = gfx_pack(ctx, rgb);
    int dx =  (x1 > x0 ? x1 - x0 : x0 - x1);
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_packed(ctx, x0, y0, p);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_rect(struct gfx_ctx *ctx, int x, int y, int w, int h, unsigned int rgb) {
    if (w <= 0 || h <= 0) return;
    gfx_line(ctx, x,         y,         x + w - 1, y,         rgb);  /* top */
    gfx_line(ctx, x,         y + h - 1, x + w - 1, y + h - 1, rgb);  /* bot */
    gfx_line(ctx, x,         y,         x,         y + h - 1, rgb);  /* left */
    gfx_line(ctx, x + w - 1, y,         x + w - 1, y + h - 1, rgb);  /* right */
}

void gfx_glyph(struct gfx_ctx *ctx, int x, int y, char c, unsigned int fg, unsigned int bg) {
    unsigned int uc = (unsigned char)c;
    if (uc < FONT_FIRST_CH || uc > FONT_LAST_CH) uc = '?';
    const uint8_t *glyph = font8x8[uc - FONT_FIRST_CH];
    unsigned int pfg = gfx_pack(ctx, fg);
    int do_bg = (bg != GFX_TRANSPARENT);
    unsigned int pbg = do_bg ? gfx_pack(ctx, bg) : 0;
    for (int r = 0; r < FONT_H; r++) {
        uint8_t bits = glyph[r];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (1u << col)) {
                put_packed(ctx, x + col, y + r, pfg);
            } else if (do_bg) {
                put_packed(ctx, x + col, y + r, pbg);
            }
        }
    }
}

void gfx_text(struct gfx_ctx *ctx, int x, int y, const char *s,
              unsigned int fg, unsigned int bg) {
    if (!s) return;
    for (int i = 0; s[i]; i++) {
        gfx_glyph(ctx, x + i * FONT_W, y, s[i], fg, bg);
    }
}

/* Session 120 — scaled glyph.  Each source pixel becomes a
 * scale*scale block.  Uses put_packed in a tight inner loop;
 * gfx_fill_rect would be cleaner for the block but the per-pixel
 * call costs nothing measurable at typical sizes and avoids re-
 * packing the colour every block. */
void gfx_glyph_n(struct gfx_ctx *ctx, int x, int y, char c,
                 int scale, unsigned int fg, unsigned int bg) {
    if (scale <= 0) return;
    if (scale == 1) { gfx_glyph(ctx, x, y, c, fg, bg); return; }
    unsigned int uc = (unsigned char)c;
    if (uc < FONT_FIRST_CH || uc > FONT_LAST_CH) uc = '?';
    const uint8_t *glyph = font8x8[uc - FONT_FIRST_CH];
    unsigned int pfg = gfx_pack(ctx, fg);
    int do_bg = (bg != GFX_TRANSPARENT);
    unsigned int pbg = do_bg ? gfx_pack(ctx, bg) : 0;
    for (int r = 0; r < FONT_H; r++) {
        uint8_t bits = glyph[r];
        for (int col = 0; col < FONT_W; col++) {
            unsigned int p;
            if (bits & (1u << col)) p = pfg;
            else if (do_bg)         p = pbg;
            else                    continue;
            int x0 = x + col * scale;
            int y0 = y + r * scale;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    put_packed(ctx, x0 + dx, y0 + dy, p);
                }
            }
        }
    }
}

void gfx_text_n(struct gfx_ctx *ctx, int x, int y, const char *s,
                int scale, unsigned int fg, unsigned int bg) {
    if (!s || scale <= 0) return;
    int step = FONT_W * scale;
    for (int i = 0; s[i]; i++) {
        gfx_glyph_n(ctx, x + i * step, y, s[i], scale, fg, bg);
    }
}
