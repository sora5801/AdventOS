/*
 * wmpair.c — session 122 sample WM client.
 *
 *   wmpair [seconds]      open TWO windows from a single process.
 *                         Default lifetime 20s.
 *
 * Demonstrates session-122's per-task VA bump in the kernel:
 * one client task allocates two distinct pixel surfaces via
 * separate SYS_WM_CREATE calls and paints into both each frame.
 *
 * The left window (cyan) shows a counter; the right window
 * (magenta) shows the same counter in a different colour scheme.
 * Closing either window via the WM close-X exits the program.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define W1_W 200
#define W1_H 100
#define W2_W 200
#define W2_H 100

static int my_atoi_str(const char *s) { return atoi(s); }

static void make_surface_ctx(struct gfx_ctx *ctx, struct wm_window *w) {
    ctx->fb       = (volatile unsigned char *)w->pixels;
    ctx->fb_real  = ctx->fb;
    ctx->back     = 0;
    ctx->user_va  = (unsigned int)(uintptr_t)w->pixels;
    ctx->width    = w->w;
    ctx->height   = w->h;
    ctx->pitch    = w->w * 4;
    ctx->bpp      = 32;
    ctx->fb_size  = w->w * w->h * 4;
}

static int format_int(char *buf, int cap, int v) {
    if (v == 0) { if (cap > 1) { buf[0] = '0'; buf[1] = 0; return 1; } return 0; }
    char t[12]; int tn = 0; int neg = v < 0;
    if (neg) v = -v;
    while (v && tn < 12) { t[tn++] = '0' + (v % 10); v /= 10; }
    int n = 0;
    if (neg && n < cap - 1) buf[n++] = '-';
    while (tn-- > 0 && n < cap - 1) buf[n++] = t[tn];
    buf[n] = 0;
    return n;
}

int main(int argc, char **argv) {
    int seconds = 20;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 20;

    struct wm_window w1, w2;
    if (wm_open(&w1, "pair-A", W1_W, W1_H) < 0) {
        printf("wmpair: 1st wm_open failed\n");
        return 1;
    }
    if (wm_open(&w2, "pair-B", W2_W, W2_H) < 0) {
        printf("wmpair: 2nd wm_open failed\n");
        wm_close(&w1);
        return 1;
    }
    printf("wmpair: w1 id=%u pixels=0x%x\n", w1.id,
           (unsigned int)(uintptr_t)w1.pixels);
    printf("wmpair: w2 id=%u pixels=0x%x\n", w2.id,
           (unsigned int)(uintptr_t)w2.pixels);

    struct gfx_ctx s1, s2;
    make_surface_ctx(&s1, &w1);
    make_surface_ctx(&s2, &w2);

    int total_ticks = seconds * 30;
    int closed = 0;
    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        /* Drain both windows' events. */
        struct wm_event ev;
        while (wm_poll_event(&w1, &ev)) {
            if (ev.type == WM_EV_CLOSE) closed = 1;
        }
        while (wm_poll_event(&w2, &ev)) {
            if (ev.type == WM_EV_CLOSE) closed = 1;
        }
        if (closed) break;

        char buf[16];
        format_int(buf, sizeof(buf), tick);

        /* Window 1 — teal background, counter centred, white text. */
        wm_clear(&w1, 0x103040u);
        wm_fill_rect(&w1, 4, 4, W1_W - 8, 12, 0x4080E0u);
        gfx_text(&s1, 8, 6, "pair-A", GFX_WHITE, GFX_TRANSPARENT);
        gfx_text(&s1, 12, 40, "tick", GFX_GREY, GFX_TRANSPARENT);
        gfx_text_n(&s1, 12, 56, buf, 2, GFX_WHITE, GFX_TRANSPARENT);

        /* Window 2 — magenta background, same counter, yellow text. */
        wm_clear(&w2, 0x301030u);
        wm_fill_rect(&w2, 4, 4, W2_W - 8, 12, 0xE030E0u);
        gfx_text(&s2, 8, 6, "pair-B", GFX_WHITE, GFX_TRANSPARENT);
        gfx_text(&s2, 12, 40, "tick", GFX_GREY, GFX_TRANSPARENT);
        gfx_text_n(&s2, 12, 56, buf, 2, GFX_YELLOW, GFX_TRANSPARENT);

        wm_present(&w1);
        wm_present(&w2);
        sys_sleep_ms(33);
    }

    wm_close(&w2);
    wm_close(&w1);
    printf("wmpair: done\n");
    return 0;
}
