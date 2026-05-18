/*
 * wmsysinfo.c — session 129 sample WM client.
 *
 *   wmsysinfo [seconds]    a small live system-info dashboard.
 *                          Default 60 s.
 *
 * Reads kernel diagnostics every second and renders a panel
 * showing: pid, CPU id, sys_time, framebuffer geometry, the
 * heap break (sys_brk(0)), and per-CPU scheduler-tick counters
 * via sys_smp_stats.
 *
 * Useful as an at-a-glance "is the system alive and healthy?"
 * display, similar to `top` but inside a WM client window.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W       400
#define WIN_H       260
#define LINE_H      12

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

/* Format unsigned int into buf; returns chars written. */
static int fmt_u(char *buf, int cap, unsigned int v) {
    char t[12]; int n = 0;
    if (v == 0) { t[n++] = '0'; }
    else { while (v && n < 12) { t[n++] = '0' + (v % 10); v /= 10; } }
    int out = 0;
    while (n-- > 0 && out < cap - 1) buf[out++] = t[n];
    buf[out] = 0;
    return out;
}

static int fmt_hex(char *buf, int cap, unsigned int v) {
    static const char hex[] = "0123456789ABCDEF";
    if (cap < 11) return 0;
    int n = 0;
    buf[n++] = '0'; buf[n++] = 'x';
    for (int i = 28; i >= 0; i -= 4) {
        buf[n++] = hex[(v >> i) & 0xF];
    }
    buf[n] = 0;
    return n;
}

/* Helper to render "label: value" on one row. */
static void draw_kv(struct gfx_ctx *sctx, int y, const char *label,
                    const char *value) {
    gfx_text(sctx, 12, y, label, 0xC0C0C0u, GFX_TRANSPARENT);
    gfx_text(sctx, 130, y, value, GFX_WHITE, GFX_TRANSPARENT);
}

int main(int argc, char **argv) {
    int seconds = 60;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 60;

    struct wm_window win;
    if (wm_open(&win, "wmsysinfo", WIN_W, WIN_H) < 0) {
        printf("wmsysinfo: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmsysinfo: id=%u\n", win.id);

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    int has_focus = 0;
    int closed    = 0;
    unsigned int t0 = sys_time();
    int total_ticks = seconds * 2;     /* 500 ms tick */

    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_CLOSE:   closed = 1; break;
                case WM_EV_KEY:
                    if (ev.keycode == 'q' || ev.keycode == 'Q'
                        || ev.keycode == 27) closed = 1;
                    break;
                default: break;
            }
        }

        wm_clear(&win, has_focus ? 0x101820u : 0x202020u);

        /* Header. */
        wm_fill_rect(&win, 0, 0, WIN_W, 18,
                     has_focus ? 0x4080E0u : 0x404040u);
        gfx_text(&sctx, 8, 5, "wmsysinfo - live kernel diagnostics",
                 GFX_WHITE, GFX_TRANSPARENT);

        /* Pull snapshots. */
        char buf[40]; int n;
        int y = 28;

        /* pid */
        n = 0; n += fmt_u(buf, sizeof(buf), (unsigned int)sys_getpid());
        draw_kv(&sctx, y, "pid:", buf);  y += LINE_H;

        /* cpu */
        n = 0; n += fmt_u(buf, sizeof(buf), (unsigned int)sys_getcpu());
        draw_kv(&sctx, y, "current cpu:", buf);  y += LINE_H;

        /* sys_time */
        unsigned int now = sys_time();
        fmt_u(buf, sizeof(buf), now);
        draw_kv(&sctx, y, "sys_time (epoch s):", buf);  y += LINE_H;

        /* uptime from this app's POV */
        fmt_u(buf, sizeof(buf), now >= t0 ? now - t0 : 0u);
        draw_kv(&sctx, y, "wmsysinfo uptime s:", buf);  y += LINE_H;

        /* heap break */
        int brk = sys_brk(0);
        fmt_hex(buf, sizeof(buf), (unsigned int)brk);
        draw_kv(&sctx, y, "heap brk:", buf);  y += LINE_H;

        /* framebuffer */
        struct sys_fb_info fb;
        if (sys_fb_info(&fb) == 0 && fb.enabled) {
            n = 0;
            n += fmt_u(buf + n, (int)sizeof(buf) - n, fb.width);
            if (n < (int)sizeof(buf) - 1) buf[n++] = 'x';
            n += fmt_u(buf + n, (int)sizeof(buf) - n, fb.height);
            if (n < (int)sizeof(buf) - 1) buf[n++] = ' ';
            if (n < (int)sizeof(buf) - 2) {
                buf[n++] = '@'; buf[n++] = ' ';
            }
            n += fmt_u(buf + n, (int)sizeof(buf) - n, fb.bpp);
            if (n < (int)sizeof(buf) - 4) {
                buf[n++] = 'b'; buf[n++] = 'p'; buf[n++] = 'p';
            }
            buf[n] = 0;
            draw_kv(&sctx, y, "framebuffer:", buf);
        } else {
            draw_kv(&sctx, y, "framebuffer:", "(disabled)");
        }
        y += LINE_H;

        /* SMP stats — 8 counters. */
        unsigned int smp[8];
        if (sys_smp_stats(smp) >= 0) {
            for (int i = 0; i < 4; i++) {
                char lbl[16];
                int li = 0;
                lbl[li++] = 'c'; lbl[li++] = 'p'; lbl[li++] = 'u'; lbl[li++] = ' ';
                lbl[li++] = '0' + (char)i;
                lbl[li++] = ':'; lbl[li] = 0;
                fmt_u(buf, sizeof(buf), smp[i]);
                draw_kv(&sctx, y, lbl, buf);
                y += LINE_H;
            }
        }

        /* Footer with quit hint. */
        gfx_text(&sctx, 8, WIN_H - 12,
                 "press q or Esc to quit; refresh every 500ms",
                 0x808080u, GFX_TRANSPARENT);

        wm_present(&win);
        sys_sleep_ms(500);
    }

    wm_close(&win);
    return 0;
}
