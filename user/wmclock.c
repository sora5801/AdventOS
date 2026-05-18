/*
 * wmclock.c — session 115 WM client.
 *
 *   wmclock [seconds]    open a clock window (default 60s).
 *
 * Shows the current time pulled from sys_time().  Space toggles
 * 12/24-hour mode.  Demonstrates a timer-driven render loop +
 * keyboard handling in the WM client model.
 *
 * The clock is drawn with a 2x-scaled 8x8 font (libgfx ships only
 * the 8x8 — bigger digits make a small window readable).
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W 260
#define WIN_H 100

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

/* Session 120 — was a custom 2x scratch-buffer expander; replaced
 * by libgfx's new gfx_text_n which takes a scale param.  The wmd
 * surface uses a synthetic gfx_ctx so gfx_text_n writes directly
 * into the shared pixel buffer with the same per-pixel cost. */

/* Format hh:mm:ss into buf.  ts = seconds since epoch.  Mode 0 = 24h,
 * 1 = 12h (with " AM" / " PM" suffix).  Buffer must hold 13 chars. */
static int format_time(char *buf, unsigned int ts, int mode_12h) {
    unsigned int sec = ts % 60u;
    unsigned int min = (ts / 60u) % 60u;
    unsigned int hr  = (ts / 3600u) % 24u;
    int am = 1;
    if (mode_12h) {
        am = (hr < 12);
        hr = hr % 12u;
        if (hr == 0) hr = 12;
    }
    int i = 0;
    buf[i++] = '0' + (char)((hr / 10) % 10);
    buf[i++] = '0' + (char)(hr % 10);
    buf[i++] = ':';
    buf[i++] = '0' + (char)(min / 10);
    buf[i++] = '0' + (char)(min % 10);
    buf[i++] = ':';
    buf[i++] = '0' + (char)(sec / 10);
    buf[i++] = '0' + (char)(sec % 10);
    if (mode_12h) {
        buf[i++] = ' ';
        buf[i++] = am ? 'A' : 'P';
        buf[i++] = 'M';
    }
    buf[i] = 0;
    return i;
}

int main(int argc, char **argv) {
    int seconds = 60;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 60;

    struct wm_window win;
    if (wm_open(&win, "Clock", WIN_W, WIN_H) < 0) {
        printf("wmclock: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmclock: id=%u (space = 12/24h)\n", win.id);

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    int mode_12h = 0;
    int has_focus = 0;
    int closed = 0;
    int total_ticks = seconds * 4;          /* 250ms tick */
    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_KEY:
                    if (ev.keycode == ' ') mode_12h = !mode_12h;
                    break;
                case WM_EV_CLOSE:   closed = 1; break;
                default: break;
            }
        }

        wm_clear(&win, has_focus ? 0x000820u : 0x080808u);

        /* Status strip. */
        wm_fill_rect(&win, 0, 0, WIN_W, 18,
                     has_focus ? 0x4080E0u : 0x303030u);
        const char *label = mode_12h ? "Clock (12h - space toggles)"
                                     : "Clock (24h - space toggles)";
        gfx_text(&sctx, 6, 5, label, GFX_WHITE, GFX_TRANSPARENT);

        /* Big time display, centered-ish. */
        unsigned int ts = sys_time();
        char tbuf[16];
        int len = format_time(tbuf, ts, mode_12h);
        int text_w = len * 16;
        int x = (WIN_W - text_w) / 2;
        int y = 32;
        gfx_text_n(&sctx, x, y, tbuf, 2, GFX_GREEN, GFX_TRANSPARENT);

        /* Footer with the raw timestamp. */
        char foot[40]; int n = 0;
        const char *p = "ts="; while (*p && n < 39) foot[n++] = *p++;
        unsigned int v = ts;
        char d[12]; int dn = 0;
        if (v == 0) d[dn++] = '0';
        else while (v) { d[dn++] = '0' + (char)(v % 10); v /= 10; }
        while (dn-- > 0 && n < 39) foot[n++] = d[dn];
        foot[n] = 0;
        gfx_text(&sctx, 6, WIN_H - 12, foot,
                 0x808080u, GFX_TRANSPARENT);

        wm_present(&win);
        sys_sleep_ms(250);
    }

    wm_close(&win);
    printf("wmclock: done\n");
    return 0;
}
