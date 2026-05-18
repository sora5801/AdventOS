/*
 * wmfiles.c — session 128 sample WM client.
 *
 *   wmfiles [seconds]    a tiny file-manager window: lists the
 *                        entries in cwd, navigates with arrow
 *                        keys, Enter descends into dirs, Backspace
 *                        cd's up.  Exits after SECONDS or 'q'.
 *
 * Demonstrates a useful real-world client: keyboard input as a
 * stateful event handler (ANSI arrow-key escape parsing),
 * filesystem syscalls (sys_readdir + sys_chdir + sys_getcwd), and
 * libgfx text rendering via a synthetic gfx_ctx pointed at the
 * libwm surface.
 *
 * The list is single-column; no scrollbar yet — overflows just
 * clip.  Selection wraps modulo entry count.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W       400
#define WIN_H       300
#define LINE_H      12
#define ROW_X       8
#define HDR_H       20
#define FOOTER_H    14
#define MAX_ENTRIES 64
#define NAME_MAX    32

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

/* Refresh the entry list from the kernel via sys_readdir.  Reads
 * from the current cwd by passing the absolute path returned by
 * sys_getcwd — the kernel's readdir resolver doesn't handle "."
 * cleanly. */
static int reload_entries(char names[MAX_ENTRIES][NAME_MAX]) {
    int iter = 0;
    int n = 0;
    char buf[16];
    char cwd[64];
    int cwd_n = sys_getcwd(cwd, (int)sizeof(cwd));
    if (cwd_n < 0) { cwd[0] = '/'; cwd[1] = 0; }
    while (n < MAX_ENTRIES) {
        int idx = sys_readdir(cwd, &iter, buf);
        if (idx < 0) break;
        int i;
        for (i = 0; i < 15 && buf[i]; i++) names[n][i] = buf[i];
        names[n][i] = 0;
        n++;
    }
    return n;
}

int main(int argc, char **argv) {
    int seconds = 60;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 60;

    struct wm_window win;
    if (wm_open(&win, "wmfiles", WIN_W, WIN_H) < 0) {
        printf("wmfiles: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmfiles: id=%u\n", win.id);

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    static char entries[MAX_ENTRIES][NAME_MAX];
    int n_entries = reload_entries(entries);
    int selected  = 0;
    int has_focus = 0;
    int closed    = 0;
    int esc_state = 0;     /* 0=normal, 1=saw ESC, 2=saw ESC [ */
    int total_ticks = seconds * 20;

    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_CLOSE:   closed = 1; break;
                case WM_EV_KEY: {
                    unsigned int k = ev.keycode;
                    /* ANSI CSI parsing: ESC '[' <final>. */
                    if (esc_state == 1) {
                        if (k == '[') { esc_state = 2; break; }
                        esc_state = 0;
                        /* lone ESC = exit */
                        closed = 1;
                        break;
                    }
                    if (esc_state == 2) {
                        esc_state = 0;
                        if (k == 'A') {        /* up */
                            if (n_entries > 0)
                                selected = (selected - 1 + n_entries) % n_entries;
                        } else if (k == 'B') { /* down */
                            if (n_entries > 0)
                                selected = (selected + 1) % n_entries;
                        }
                        /* C/D (left/right) ignored */
                        break;
                    }
                    if (k == 27) {             /* ESC start */
                        esc_state = 1;
                        break;
                    }
                    if (k == '\n' || k == '\r') {
                        /* Enter — try to cd. Fails harmlessly for
                         * non-dir entries. */
                        if (n_entries > 0 && selected < n_entries) {
                            if (sys_chdir(entries[selected]) == 0) {
                                n_entries = reload_entries(entries);
                                selected = 0;
                            }
                        }
                    } else if (k == 0x08 || k == 0x7F) {
                        /* Backspace — cd .. */
                        if (sys_chdir("..") == 0) {
                            n_entries = reload_entries(entries);
                            selected = 0;
                        }
                    } else if (k == 'q' || k == 'Q') {
                        closed = 1;
                    } else if (k == 'r' || k == 'R') {
                        n_entries = reload_entries(entries);
                        if (selected >= n_entries) selected = 0;
                    }
                    break;
                }
                case WM_EV_MOUSE_PRESS: {
                    /* Clicking on a list row selects it. */
                    int idx = (ev.y - HDR_H) / LINE_H;
                    if (idx >= 0 && idx < n_entries) selected = idx;
                    break;
                }
                default: break;
            }
        }

        /* Repaint. */
        wm_clear(&win, has_focus ? 0x101820u : 0x202020u);

        /* Header — current path. */
        wm_fill_rect(&win, 0, 0, WIN_W, HDR_H,
                     has_focus ? 0x4080E0u : 0x404040u);
        char cwd[64];
        int cwd_n = sys_getcwd(cwd, (int)sizeof(cwd));
        if (cwd_n < 0) { cwd[0] = '?'; cwd[1] = 0; }
        gfx_text(&sctx, 6, 6, cwd, GFX_WHITE, GFX_TRANSPARENT);

        /* Entry list. */
        int max_rows = (WIN_H - HDR_H - FOOTER_H) / LINE_H;
        int row0 = 0;
        if (selected >= row0 + max_rows) row0 = selected - max_rows + 1;
        for (int r = 0; r < max_rows && row0 + r < n_entries; r++) {
            int idx = row0 + r;
            int y   = HDR_H + 4 + r * LINE_H;
            int sel = (idx == selected);
            if (sel) {
                wm_fill_rect(&win, 2, y - 1, WIN_W - 4,
                             LINE_H, 0x405880u);
            }
            gfx_text(&sctx, ROW_X, y + 2, entries[idx],
                     sel ? GFX_WHITE : 0xC0C0C0u, GFX_TRANSPARENT);
        }

        /* Footer. */
        gfx_text(&sctx, 6, WIN_H - 11,
                 "arrows + Enter to navigate; bksp = up; q = quit",
                 0x808080u, GFX_TRANSPARENT);

        wm_present(&win);
        sys_sleep_ms(50);
    }

    wm_close(&win);
    return 0;
}
