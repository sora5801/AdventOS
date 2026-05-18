/*
 * wmps.c — session 130 sample WM client.
 *
 *   wmps [seconds]    open a window that lists the running
 *                     processes (pid, state, name) refreshed
 *                     every second.  Default 60 s.
 *
 * Reads /proc/<pid>/status for each numeric entry under /proc —
 * same source as the CLI `ps` program, just rendered into a
 * libwm surface instead of stdout.  Up/Down arrows move the
 * selection but selection is just visual; no actions yet.
 * Press 'r' to refresh on demand, 'q' / Esc to quit.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W       420
#define WIN_H       320
#define LINE_H      12
#define HDR_H       20
#define FOOTER_H    14
#define MAX_ROWS    32

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

struct proc_row {
    int  pid;
    char name [24];
    char state[16];
};
static struct proc_row g_rows[MAX_ROWS];
static int             g_n_rows;

static int my_atoi_pos(const char *s) {
    if (!s) return -1;
    int n = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

static int read_status(int pid, char *buf, int cap) {
    /* Build "/proc/<pid>/status" by hand. */
    char path[40];
    int i = 0;
    const char *p = "/proc/";
    while (*p && i < (int)sizeof(path) - 1) path[i++] = *p++;
    /* itoa pid */
    char t[12]; int tn = 0; int v = pid;
    if (v == 0) t[tn++] = '0';
    else while (v && tn < 12) { t[tn++] = '0' + v % 10; v /= 10; }
    while (tn-- > 0 && i < (int)sizeof(path) - 1) path[i++] = t[tn];
    const char *suf = "/status";
    while (*suf && i < (int)sizeof(path) - 1) path[i++] = *suf++;
    path[i] = 0;

    int fd = sys_open(path);
    if (fd < 0) return -1;
    int total = 0;
    while (total < cap - 1) {
        int got = sys_read(fd, buf + total, cap - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    buf[total] = 0;
    sys_close(fd);
    return total;
}

/* Find "Name: <value>\n" or "State: <value>\n" in `buf` and copy
 * `<value>` into `out` (NUL-terminated, capped). */
static void extract(const char *buf, const char *key, char *out, int cap) {
    int klen = 0; while (key[klen]) klen++;
    int i = 0;
    while (buf[i]) {
        /* Check if buf[i..] starts with key. */
        int m = 1;
        for (int j = 0; j < klen; j++) {
            if (buf[i + j] != key[j]) { m = 0; break; }
        }
        if (m) {
            int p = i + klen;
            while (buf[p] == ' ' || buf[p] == '\t') p++;
            int o = 0;
            while (buf[p] && buf[p] != '\n' && o < cap - 1) {
                out[o++] = buf[p++];
            }
            out[o] = 0;
            return;
        }
        i++;
    }
    out[0] = 0;
}

static int gather(void) {
    int n = 0;
    int iter = 0;
    char name[17];
    while (n < MAX_ROWS) {
        for (int i = 0; i < 17; i++) name[i] = 0;
        int idx = sys_readdir("/proc", &iter, name);
        if (idx < 0) break;
        int pid = my_atoi_pos(name);
        if (pid <= 0) continue;
        char sbuf[256];
        int sz = read_status(pid, sbuf, (int)sizeof(sbuf));
        if (sz <= 0) continue;
        g_rows[n].pid = pid;
        extract(sbuf, "Name:",  g_rows[n].name,  sizeof(g_rows[n].name));
        extract(sbuf, "State:", g_rows[n].state, sizeof(g_rows[n].state));
        n++;
    }
    return n;
}

static int fmt_int(char *buf, int cap, int v) {
    char t[12]; int tn = 0;
    if (v == 0) t[tn++] = '0';
    else while (v && tn < 12) { t[tn++] = '0' + (v % 10); v /= 10; }
    int n = 0;
    while (tn-- > 0 && n < cap - 1) buf[n++] = t[tn];
    buf[n] = 0;
    return n;
}

int main(int argc, char **argv) {
    int seconds = 60;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 60;

    struct wm_window win;
    if (wm_open(&win, "wmps", WIN_W, WIN_H) < 0) {
        printf("wmps: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmps: id=%u\n", win.id);

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    g_n_rows = gather();
    int selected = 0;
    int has_focus = 0;
    int closed = 0;
    int esc_state = 0;
    int total_ticks = seconds * 2;          /* 500 ms refresh */
    int refresh_in = 0;

    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_CLOSE:   closed = 1; break;
                case WM_EV_KEY: {
                    unsigned int k = ev.keycode;
                    if (esc_state == 1) {
                        if (k == '[') { esc_state = 2; break; }
                        esc_state = 0; closed = 1; break;
                    }
                    if (esc_state == 2) {
                        esc_state = 0;
                        if (k == 'A' && g_n_rows > 0)
                            selected = (selected - 1 + g_n_rows) % g_n_rows;
                        else if (k == 'B' && g_n_rows > 0)
                            selected = (selected + 1) % g_n_rows;
                        break;
                    }
                    if (k == 27) { esc_state = 1; break; }
                    if (k == 'q' || k == 'Q') closed = 1;
                    else if (k == 'r' || k == 'R') {
                        g_n_rows = gather();
                        if (selected >= g_n_rows) selected = 0;
                    }
                    break;
                }
                case WM_EV_MOUSE_PRESS: {
                    int idx = (ev.y - HDR_H) / LINE_H;
                    if (idx >= 0 && idx < g_n_rows) selected = idx;
                    break;
                }
                default: break;
            }
        }

        /* Auto-refresh every other tick (= 1 s). */
        if (refresh_in <= 0) {
            g_n_rows = gather();
            if (selected >= g_n_rows && g_n_rows > 0)
                selected = g_n_rows - 1;
            refresh_in = 2;
        }
        refresh_in--;

        /* Repaint. */
        wm_clear(&win, has_focus ? 0x101820u : 0x202020u);

        wm_fill_rect(&win, 0, 0, WIN_W, HDR_H,
                     has_focus ? 0x4080E0u : 0x404040u);
        gfx_text(&sctx, 8, 5,
                 "wmps - processes (q=quit r=refresh)",
                 GFX_WHITE, GFX_TRANSPARENT);

        /* Column headers. */
        int hdr_y = HDR_H + 4;
        gfx_text(&sctx, 8,   hdr_y, "PID",  0xA0A0A0u, GFX_TRANSPARENT);
        gfx_text(&sctx, 60,  hdr_y, "STATE",0xA0A0A0u, GFX_TRANSPARENT);
        gfx_text(&sctx, 130, hdr_y, "NAME", 0xA0A0A0u, GFX_TRANSPARENT);
        wm_fill_rect(&win, 2, hdr_y + LINE_H + 2,
                     WIN_W - 4, 1, 0x404850u);

        /* Rows. */
        int row_y0 = hdr_y + LINE_H + 6;
        int max_rows = (WIN_H - row_y0 - FOOTER_H) / LINE_H;
        int row0 = 0;
        if (selected >= row0 + max_rows) row0 = selected - max_rows + 1;
        for (int r = 0; r < max_rows && row0 + r < g_n_rows; r++) {
            int idx = row0 + r;
            int y   = row_y0 + r * LINE_H;
            int sel = (idx == selected);
            if (sel) {
                wm_fill_rect(&win, 2, y - 1, WIN_W - 4,
                             LINE_H, 0x405880u);
            }
            unsigned int fg = sel ? GFX_WHITE : 0xC0C0C0u;
            char buf[16];
            fmt_int(buf, sizeof(buf), g_rows[idx].pid);
            gfx_text(&sctx, 8,   y + 2, buf,             fg, GFX_TRANSPARENT);
            gfx_text(&sctx, 60,  y + 2, g_rows[idx].state, fg, GFX_TRANSPARENT);
            gfx_text(&sctx, 130, y + 2, g_rows[idx].name,  fg, GFX_TRANSPARENT);
        }

        /* Footer. */
        char foot[40]; int fn = 0;
        const char *p = "rows="; while (*p && fn < 39) foot[fn++] = *p++;
        fn += fmt_int(foot + fn, (int)sizeof(foot) - fn, g_n_rows);
        foot[fn] = 0;
        gfx_text(&sctx, 8, WIN_H - 12, foot,
                 0x808080u, GFX_TRANSPARENT);

        wm_present(&win);
        sys_sleep_ms(500);
    }

    wm_close(&win);
    return 0;
}
