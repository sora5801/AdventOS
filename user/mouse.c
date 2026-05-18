/*
 * mouse.c — session 109 / Path C phase 3 mouse smoke test.
 *
 *   mouse [seconds]    take over the framebuffer; draw a moving
 *                      crosshair at the current mouse position
 *                      until SECONDS elapse (default 8), then
 *                      release.
 *
 * Each frame: erase last cursor (with the static backdrop), redraw
 * cursor at new position, blit a small HUD with x/y/buttons.
 * Direct-store (no backbuffer yet — session 110), so the cursor
 * draws on top of whatever's there with occasional sparkle as the
 * scan races our write.
 */

#include "libuser.h"
#include "../libgfx/libgfx.h"

#define FB_VA  0x50000000u
#define CURSOR_R 8

/* Draw a small crosshair-style cursor centered at (cx, cy). Caller
 * supplies the color; we don't worry about clipping the lines since
 * gfx_line clips per-pixel. */
static void draw_cursor(struct gfx_ctx *ctx, int cx, int cy, unsigned int rgb) {
    gfx_line(ctx, cx - CURSOR_R, cy, cx + CURSOR_R, cy, rgb);
    gfx_line(ctx, cx, cy - CURSOR_R, cx, cy + CURSOR_R, rgb);
}

static int my_atoi(const char *s) { return atoi(s); }

int main(int argc, char **argv) {
    int seconds = 8;
    if (argc >= 2) seconds = my_atoi(argv[1]);
    if (seconds <= 0) seconds = 8;

    struct gfx_ctx ctx;
    /* Session 110 — use double-buffered mode. Each frame: clear back,
     * draw everything, gfx_present. No more per-frame erase-then-draw
     * — tearing is gone because the present is a single blit. */
    if (gfx_init_db(&ctx, FB_VA) < 0) {
        printf("mouse: framebuffer unavailable or already owned\n");
        return 1;
    }

    int hud_y = (int)ctx.height - 32;
    int total_ticks = seconds * 60;
    for (int tick = 0; tick < total_ticks; tick++) {
        struct sys_mouse_state ms;
        if (sys_mouse_poll(&ms) < 0) break;

        /* Repaint the whole frame each tick — the backbuffer makes
         * it cheap and the present is atomic from the viewer's
         * perspective. */
        gfx_clear(&ctx, GFX_DARK_GREY);
        gfx_text(&ctx, 8, 8, "AdventOS Path C - sessions 109+110",
                 GFX_WHITE, GFX_TRANSPARENT);
        gfx_text(&ctx, 8, 24,
                 "move the mouse; double-buffered: no tearing now",
                 GFX_GREY, GFX_TRANSPARENT);
        gfx_rect(&ctx, 4, 4, (int)ctx.width - 8, (int)ctx.height - 8, GFX_GREEN);

        unsigned int color = GFX_WHITE;
        if (ms.buttons & 0x01) color = GFX_RED;
        else if (ms.buttons & 0x02) color = GFX_BLUE;
        else if (ms.buttons & 0x04) color = GFX_YELLOW;
        draw_cursor(&ctx, ms.x, ms.y, color);

        /* HUD strip. */
        gfx_fill_rect(&ctx, 4, hud_y, (int)ctx.width - 8, 28, GFX_BLACK);
        char buf[80];
        int n = 0;
        const char *p = "x=";
        while (*p && n < (int)sizeof(buf) - 1) buf[n++] = *p++;
        { char t[12]; int tn = 0; int v = ms.x; int neg = v < 0;
          if (neg) v = -v;
          if (v == 0) t[tn++] = '0';
          else while (v) { t[tn++] = '0' + v % 10; v /= 10; }
          if (neg && n < (int)sizeof(buf) - 1) buf[n++] = '-';
          while (tn-- > 0 && n < (int)sizeof(buf) - 1) buf[n++] = t[tn]; }
        p = "  y=";
        while (*p && n < (int)sizeof(buf) - 1) buf[n++] = *p++;
        { char t[12]; int tn = 0; int v = ms.y; int neg = v < 0;
          if (neg) v = -v;
          if (v == 0) t[tn++] = '0';
          else while (v) { t[tn++] = '0' + v % 10; v /= 10; }
          if (neg && n < (int)sizeof(buf) - 1) buf[n++] = '-';
          while (tn-- > 0 && n < (int)sizeof(buf) - 1) buf[n++] = t[tn]; }
        p = "  buttons=";
        while (*p && n < (int)sizeof(buf) - 1) buf[n++] = *p++;
        if (n < (int)sizeof(buf) - 1) buf[n++] = (ms.buttons & 0x01) ? 'L' : '-';
        if (n < (int)sizeof(buf) - 1) buf[n++] = (ms.buttons & 0x02) ? 'R' : '-';
        if (n < (int)sizeof(buf) - 1) buf[n++] = (ms.buttons & 0x04) ? 'M' : '-';
        buf[n] = 0;
        gfx_text(&ctx, 12, hud_y + 10, buf, GFX_GREEN, GFX_BLACK);

        /* Push the backbuffer to the real FB in one go. */
        gfx_present(&ctx);
        sys_sleep_ms(16);
    }

    gfx_release(&ctx);
    printf("mouse: released\n");
    return 0;
}
