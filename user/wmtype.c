/*
 * wmtype.c — session 114 sample WM client.
 *
 *   wmtype [seconds]    open a 320x200 window and act as a tiny
 *                       text scratchpad.  Whatever you type into
 *                       the focused window appears.  Backspace
 *                       deletes the last char, Enter wraps line.
 *                       Exit after SECONDS (default 30).
 *
 * Demonstrates:
 *   - keyboard event delivery via WM_EV_KEY  (session 114)
 *   - 8x8-font text rendering into a libwm surface using a
 *     synthetic gfx_ctx pointed at the client's pixel buffer
 *
 * Click the wmtype window first (so wmd's click-focus targets it),
 * then type.  Keystrokes that arrive while focus is on a different
 * window go to that window, not wmtype.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W 320
#define WIN_H 200
#define BUF_MAX 1024

static int my_atoi_str(const char *s) { return atoi(s); }

/* Construct a one-shot gfx_ctx that paints into the libwm surface
 * (always 32-bit packed).  libgfx's gfx_text/gfx_glyph use this. */
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

int main(int argc, char **argv) {
    int seconds = 30;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 30;

    struct wm_window win;
    if (wm_open(&win, "wmtype", WIN_W, WIN_H) < 0) {
        printf("wmtype: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmtype: id=%u — click the window then type\n", win.id);

    static char buf[BUF_MAX];
    int len = 0;
    int has_focus = 0;
    int total_ticks = seconds * 30;
    int caret_phase = 0;
    int closed = 0;

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        /* Drain events. */
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_KEY: {
                    unsigned int k = ev.keycode;
                    if (k == 0x08 || k == 0x7F) {        /* backspace / DEL */
                        if (len > 0) len--;
                    } else if (k == '\r' || k == '\n') {
                        if (len < BUF_MAX - 1) buf[len++] = '\n';
                    } else if (k >= 0x20 && k <= 0x7E) {  /* printable */
                        if (len < BUF_MAX - 1) buf[len++] = (char)k;
                    }
                    /* Other keys (arrows, ctrl, ...) ignored for now. */
                    break;
                }
                case WM_EV_MOUSE_PRESS:
                    /* Click-to-clear if the user double-clicks at
                     * the top-right "X" area (a 14x14 region within
                     * the *content* area; this is wmtype's own
                     * widget, not the WM's close button which is in
                     * the title bar above). */
                    if (ev.x >= WIN_W - 18 && ev.x < WIN_W - 4 &&
                        ev.y >= 2 && ev.y < 16) {
                        len = 0;
                    }
                    break;
                case WM_EV_CLOSE:
                    closed = 1;
                    break;
                default: break;
            }
        }

        /* Paint. */
        wm_clear(&win, has_focus ? 0x101820u : 0x202020u);

        /* Status bar at the top. */
        wm_fill_rect(&win, 0, 0, WIN_W, 18,
                     has_focus ? 0x4080E0u : 0x404040u);
        gfx_text(&sctx, 6, 5,
                 has_focus ? "wmtype - click to type"
                           : "wmtype - click to focus",
                 GFX_WHITE, GFX_TRANSPARENT);

        /* "Close" hint at top-right. */
        wm_fill_rect(&win, WIN_W - 18, 2, 14, 14, 0xE03030u);
        gfx_text(&sctx, WIN_W - 15, 5, "X", GFX_WHITE, GFX_TRANSPARENT);

        /* Render the text buffer below the status bar.  Wrap by
         * column count (FONT_W=8 → WIN_W/8 = 40 chars per line). */
        const int char_w = 8;
        const int char_h = 10;
        const int text_y0 = 24;
        const int max_cols = (WIN_W - 8) / char_w;
        int col = 0, row = 0;
        for (int i = 0; i < len; i++) {
            char c = buf[i];
            if (c == '\n' || col >= max_cols) {
                col = 0; row++;
                if (c == '\n') continue;
            }
            int x = 4 + col * char_w;
            int y = text_y0 + row * char_h;
            if (y >= WIN_H - 12) break;
            gfx_glyph(&sctx, x, y, c, GFX_WHITE, GFX_TRANSPARENT);
            col++;
        }

        /* Blinking caret. */
        if (has_focus && ((caret_phase / 15) & 1) == 0) {
            int cx = 4 + col * char_w;
            int cy = text_y0 + row * char_h;
            if (cy < WIN_H - 12) {
                wm_fill_rect(&win, cx, cy, 6, char_h - 2, 0xFFFFFFu);
            }
        }
        caret_phase++;

        /* Footer status line. */
        char info[40];
        int n = 0;
        const char *p = "chars="; while (*p && n < 39) info[n++] = *p++;
        unsigned int v = (unsigned int)len;
        char t[8]; int tn = 0;
        if (v == 0) t[tn++] = '0';
        else while (v) { t[tn++] = '0' + (v % 10); v /= 10; }
        while (tn-- > 0 && n < 39) info[n++] = t[tn];
        info[n] = 0;
        gfx_text(&sctx, 4, WIN_H - 12, info, 0x808080u, GFX_TRANSPARENT);

        wm_present(&win);
        sys_sleep_ms(33);
    }

    printf("wmtype: typed %d chars; closing\n", len);
    wm_close(&win);
    return 0;
}
