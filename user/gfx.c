/*
 * gfx.c — session 108 update.
 *
 *   gfx [seconds]    paint a test card; sleep `seconds` (default 4);
 *                    release the FB and let fbcon resume.
 *
 * As of session 108 this is built on top of libgfx — the per-pixel
 * helpers, font rendering, Bresenham line, etc. live there.
 */

#include "libuser.h"
#include "../libgfx/libgfx.h"

#define FB_VA  0x50000000u   /* well clear of stack/heap/code */

int main(int argc, char **argv) {
    int seconds = 4;
    if (argc >= 2) seconds = atoi(argv[1]);
    if (seconds <= 0) seconds = 4;

    struct gfx_ctx ctx;
    if (gfx_init(&ctx, FB_VA) < 0) {
        printf("gfx: framebuffer unavailable or already owned\n");
        return 1;
    }
    printf("gfx: %ux%u %ubpp, pitch=%u, %u KiB\n",
           ctx.width, ctx.height, ctx.bpp, ctx.pitch, ctx.fb_size >> 10);

    gfx_clear(&ctx, GFX_DARK_GREY);

    /* Color bars across the top 64 px. */
    unsigned int colors[8] = {
        0xFF0000, 0xFF7F00, 0xFFFF00, 0x00FF00,
        0x00FFFF, 0x0000FF, 0x8000FF, 0xFFFFFF,
    };
    int bar_w = (int)ctx.width / 8;
    for (int i = 0; i < 8; i++) {
        gfx_fill_rect(&ctx, i * bar_w, 0, bar_w, 64, colors[i]);
    }

    /* RGB gradient block in the upper-left (below the bars). */
    int gx0 = 32, gy0 = 96, gw = 256, gh = 256;
    for (int yy = 0; yy < gh; yy++) {
        for (int xx = 0; xx < gw; xx++) {
            unsigned int r = (unsigned int)xx;
            unsigned int g = (unsigned int)yy;
            unsigned int b = (unsigned int)((xx + yy) >> 1);
            gfx_put_pixel(&ctx, gx0 + xx, gy0 + yy,
                          (r << 16) | (g << 8) | b);
        }
    }

    /* Centered filled rectangle. */
    int rw = 240, rh = 140;
    int rx = ((int)ctx.width  - rw) / 2;
    int ry = ((int)ctx.height - rh) / 2;
    gfx_fill_rect(&ctx, rx, ry, rw, rh, 0x40A0F0);
    gfx_rect(&ctx, rx, ry, rw, rh, GFX_WHITE);
    gfx_text(&ctx, rx + 16, ry + 12, "AdventOS Path C",
             GFX_WHITE, GFX_TRANSPARENT);
    gfx_text(&ctx, rx + 16, ry + 28, "session 108: libgfx",
             GFX_WHITE, GFX_TRANSPARENT);
    /* libuser doesn't ship snprintf; format manually. */
    char dim_buf[64];
    int n = 0;
    const char *prefix = "fb: ";
    for (; prefix[n] && n < (int)sizeof(dim_buf) - 1; n++) dim_buf[n] = prefix[n];
    {
        char tmp[16]; int tn = 0;
        unsigned int v = ctx.width;
        if (v == 0) tmp[tn++] = '0';
        else while (v) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        while (tn-- > 0 && n < (int)sizeof(dim_buf) - 1) dim_buf[n++] = tmp[tn];
    }
    if (n < (int)sizeof(dim_buf) - 1) dim_buf[n++] = 'x';
    {
        char tmp[16]; int tn = 0;
        unsigned int v = ctx.height;
        if (v == 0) tmp[tn++] = '0';
        else while (v) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        while (tn-- > 0 && n < (int)sizeof(dim_buf) - 1) dim_buf[n++] = tmp[tn];
    }
    const char *suf = " @ ";
    for (int s = 0; suf[s] && n < (int)sizeof(dim_buf) - 1; s++) dim_buf[n++] = suf[s];
    {
        char tmp[8]; int tn = 0;
        unsigned int v = ctx.bpp;
        if (v == 0) tmp[tn++] = '0';
        else while (v) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        while (tn-- > 0 && n < (int)sizeof(dim_buf) - 1) dim_buf[n++] = tmp[tn];
    }
    const char *tail = " bpp";
    for (int s = 0; tail[s] && n < (int)sizeof(dim_buf) - 1; s++) dim_buf[n++] = tail[s];
    dim_buf[n] = 0;
    gfx_text(&ctx, rx + 16, ry + 44, dim_buf, GFX_WHITE, GFX_TRANSPARENT);

    /* Two diagonals across the screen. */
    gfx_line(&ctx, 0,                 0,
                   (int)ctx.width - 1, (int)ctx.height - 1, GFX_WHITE);
    gfx_line(&ctx, (int)ctx.width - 1, 0,
                   0,                  (int)ctx.height - 1, GFX_WHITE);

    /* Bottom strip of labels. */
    int by = (int)ctx.height - 28;
    gfx_fill_rect(&ctx, 0, by, (int)ctx.width, 28, GFX_BLACK);
    gfx_text(&ctx, 8, by + 8,
             "libgfx: clear / fill_rect / put_pixel / line / rect / glyph / text",
             GFX_GREEN, GFX_BLACK);

    for (int s = 0; s < seconds; s++) sys_sleep_ms(1000);

    gfx_release(&ctx);
    printf("gfx: released\n");
    return 0;
}
