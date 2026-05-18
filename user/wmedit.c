/*
 * wmedit.c — session 137 / Path C phase 30 text editor.
 *
 *   wmedit [path] [seconds]
 *
 *      [path]      file to open/edit.  If omitted, starts with an
 *                  empty buffer that Ctrl+S saves to a default
 *                  location.  Non-existent paths open as a new
 *                  buffer that Ctrl+S will create.
 *      [seconds]   lifetime; default 600 (10 min).  Ctrl+Q exits
 *                  early.
 *
 * Keys (focused window only):
 *   printable ASCII   insert at cursor
 *   Enter             insert '\n' (newline)
 *   Backspace / DEL   delete the char before the cursor
 *   Arrow keys        move cursor up/down/left/right
 *   Ctrl+S            save buffer to current path
 *   Ctrl+Q            quit
 *   Ctrl+C            copy entire buffer to the global clipboard
 *   Ctrl+V            paste clipboard contents at the cursor
 *
 * Status bar (bottom of window) shows:
 *   "<path>  [*]  L<row>:<col>"
 * where [*] appears iff the buffer has unsaved changes.
 *
 * No selection, no search, no undo, no file picker — those would
 * be larger UI features.  This is enough to load text, edit it,
 * save it, and round-trip text through the clipboard.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W       640
#define WIN_H       400
#define HDR_H       18
#define FOOTER_H    14
#define CELL_W      8
#define LINE_H      10
#define GRID_X      6
#define BUF_MAX     8192
#define DEFAULT_PATH "/tmp/edit"

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

static char  g_buf[BUF_MAX];
static int   g_len;
static int   g_cur;          /* byte offset into g_buf */
static int   g_dirty;        /* 1 if buffer modified since load/save */
static char  g_path[64];

/* Compute (row, col) for a given byte offset. */
static void cursor_rowcol(int off, int *row, int *col) {
    int r = 0, c = 0;
    if (off > g_len) off = g_len;
    for (int i = 0; i < off; i++) {
        if (g_buf[i] == '\n') { r++; c = 0; }
        else                   { c++; }
    }
    *row = r;
    *col = c;
}

/* Inverse: byte offset of the start of `row` (0..). */
static int row_start_off(int row) {
    if (row <= 0) return 0;
    int r = 0;
    for (int i = 0; i < g_len; i++) {
        if (g_buf[i] == '\n') {
            r++;
            if (r == row) return i + 1;
        }
    }
    return g_len;
}

/* Try to load the file at g_path into g_buf. */
static int load_file(void) {
    int fd = sys_open(g_path);
    if (fd < 0) {
        g_len = 0; g_cur = 0; g_dirty = 0;
        return -1;
    }
    int n = sys_read(fd, g_buf, BUF_MAX);
    sys_close(fd);
    if (n < 0) n = 0;
    g_len = n;
    g_cur = 0;
    g_dirty = 0;
    return 0;
}

static int save_file(void) {
    if (sys_fs_write(g_path, g_buf, (uint32_t)g_len) != 0) return -1;
    g_dirty = 0;
    return 0;
}

/* Insert one byte at cursor; advance cursor. */
static void buf_insert(char c) {
    if (g_len >= BUF_MAX - 1) return;
    for (int i = g_len; i > g_cur; i--) g_buf[i] = g_buf[i - 1];
    g_buf[g_cur] = c;
    g_len++;
    g_cur++;
    g_dirty = 1;
}

static void buf_delete(void) {
    if (g_cur == 0) return;
    for (int i = g_cur - 1; i < g_len - 1; i++) g_buf[i] = g_buf[i + 1];
    g_len--;
    g_cur--;
    g_dirty = 1;
}

/* Move cursor by direction.  d=='A' up, 'B' down, 'C' right, 'D' left. */
static void cursor_move(char d) {
    int row, col;
    cursor_rowcol(g_cur, &row, &col);
    if (d == 'C') {
        if (g_cur < g_len) g_cur++;
    } else if (d == 'D') {
        if (g_cur > 0) g_cur--;
    } else if (d == 'A' && row > 0) {
        int dst_row = row - 1;
        int start = row_start_off(dst_row);
        int end   = row_start_off(dst_row + 1);
        if (end > start && g_buf[end - 1] == '\n') end--;
        int new_col = col;
        if (new_col > end - start) new_col = end - start;
        g_cur = start + new_col;
    } else if (d == 'B') {
        int dst_row = row + 1;
        int start = row_start_off(dst_row);
        if (start >= g_len && dst_row > 0) return;   /* no row below */
        int end = row_start_off(dst_row + 1);
        if (end > start && g_buf[end - 1] == '\n') end--;
        int new_col = col;
        if (new_col > end - start) new_col = end - start;
        g_cur = start + new_col;
    }
}

/* Render `s` left-aligned at (x, y).  Returns next x.  Stops at
 * NUL. */
static int draw_str(struct gfx_ctx *sctx, int x, int y,
                    const char *s, unsigned int rgb) {
    for (; *s; s++) {
        gfx_glyph(sctx, x, y, *s, rgb, GFX_TRANSPARENT);
        x += CELL_W;
    }
    return x;
}

/* Decimal printer.  Appends to buf (NUL-terminated); returns chars
 * written. */
static int dec(char *buf, int cap, unsigned int v) {
    char t[12]; int n = 0;
    if (v == 0) { t[n++] = '0'; }
    else { while (v && n < 12) { t[n++] = '0' + (v % 10); v /= 10; } }
    int out = 0;
    while (n-- > 0 && out < cap - 1) buf[out++] = t[n];
    buf[out] = 0;
    return out;
}

int main(int argc, char **argv) {
    int seconds = 600;

    /* argv layout: argv[1] = path (optional), argv[2] = seconds. */
    if (argc >= 2 && argv[1][0]) {
        int i = 0;
        for (; i < (int)sizeof(g_path) - 1 && argv[1][i]; i++)
            g_path[i] = argv[1][i];
        g_path[i] = 0;
    } else {
        const char *def = DEFAULT_PATH;
        int i = 0;
        for (; def[i] && i < (int)sizeof(g_path) - 1; i++)
            g_path[i] = def[i];
        g_path[i] = 0;
    }
    if (argc >= 3) seconds = my_atoi_str(argv[2]);
    if (seconds <= 0) seconds = 600;

    /* Attempt to load.  No file = empty buffer (Ctrl+S will create). */
    (void)load_file();

    struct wm_window win;
    if (wm_open(&win, "wmedit", WIN_W, WIN_H) < 0) {
        printf("wmedit: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmedit: id=%u path=%s len=%d\n", win.id, g_path, g_len);

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    int has_focus  = 0;
    int closed     = 0;
    int esc_state  = 0;       /* 0 normal, 1 saw ESC, 2 saw ESC[ */
    int caret_phase = 0;
    int total_ticks = seconds * 30;

    const int max_rows = (WIN_H - HDR_H - FOOTER_H - 8) / LINE_H;
    const int max_cols = (WIN_W - 2 * GRID_X) / CELL_W;
    int scroll_row = 0;

    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_CLOSE:   closed = 1; break;
                case WM_EV_KEY: {
                    unsigned int k = ev.keycode;
                    /* ANSI CSI: ESC '[' <final>. */
                    if (esc_state == 1) {
                        if (k == '[') { esc_state = 2; break; }
                        esc_state = 0;
                        break;
                    }
                    if (esc_state == 2) {
                        esc_state = 0;
                        if (k == 'A' || k == 'B' || k == 'C' || k == 'D')
                            cursor_move((char)k);
                        break;
                    }
                    if (k == 27) { esc_state = 1; break; }
                    if (k == 0x13) {              /* Ctrl+S — save */
                        save_file();
                    } else if (k == 0x11) {       /* Ctrl+Q — quit */
                        closed = 1;
                    } else if (k == 0x03) {       /* Ctrl+C — copy */
                        wm_clipboard_set(g_buf, g_len);
                    } else if (k == 0x16) {       /* Ctrl+V — paste */
                        char pb[BUF_MAX];
                        int pn = wm_clipboard_get(pb, (int)sizeof(pb));
                        if (pn > 0) {
                            if (pn > BUF_MAX - g_len - 1) pn = BUF_MAX - g_len - 1;
                            for (int i = 0; i < pn; i++) buf_insert(pb[i]);
                        }
                    } else if (k == 0x08 || k == 0x7F) {
                        buf_delete();
                    } else if (k == '\r' || k == '\n') {
                        buf_insert('\n');
                    } else if (k == '\t') {
                        for (int i = 0; i < 4; i++) buf_insert(' ');
                    } else if (k >= 0x20 && k <= 0x7E) {
                        buf_insert((char)k);
                    }
                    break;
                }
                case WM_EV_MOUSE_PRESS: {
                    /* Click in the text region: position cursor by
                     * mapping pixel (x,y) to a (row, col) within the
                     * current viewport. */
                    int rel_y = ev.y - HDR_H - 4;
                    int rel_x = ev.x - GRID_X;
                    if (rel_y < 0 || rel_x < 0) break;
                    int row = scroll_row + rel_y / LINE_H;
                    int col = rel_x / CELL_W;
                    int start = row_start_off(row);
                    int end   = row_start_off(row + 1);
                    if (end > start && g_buf[end - 1] == '\n') end--;
                    if (col > end - start) col = end - start;
                    g_cur = start + col;
                    break;
                }
                default: break;
            }
        }

        /* Scroll so the cursor stays in view. */
        int cur_row, cur_col;
        cursor_rowcol(g_cur, &cur_row, &cur_col);
        if (cur_row < scroll_row) scroll_row = cur_row;
        if (cur_row >= scroll_row + max_rows)
            scroll_row = cur_row - max_rows + 1;
        if (scroll_row < 0) scroll_row = 0;

        /* Repaint. */
        wm_clear(&win, has_focus ? 0x0A0A14u : 0x141414u);

        /* Title bar. */
        wm_fill_rect(&win, 0, 0, WIN_W, HDR_H,
                     has_focus ? 0x4080E0u : 0x404040u);
        gfx_text(&sctx, 6, 5,
                 has_focus ? "wmedit  Ctrl-S save  Ctrl-Q quit"
                           : "wmedit  (click to focus)",
                 GFX_WHITE, GFX_TRANSPARENT);

        /* Text body. */
        int text_y0 = HDR_H + 4;
        int byte = row_start_off(scroll_row);
        for (int r = 0; r < max_rows; r++) {
            int y = text_y0 + r * LINE_H;
            int col = 0;
            while (byte < g_len && g_buf[byte] != '\n') {
                if (col < max_cols) {
                    gfx_glyph(&sctx, GRID_X + col * CELL_W, y,
                              g_buf[byte], 0xC0E0C0u, GFX_TRANSPARENT);
                }
                col++;
                byte++;
            }
            if (byte < g_len && g_buf[byte] == '\n') byte++;
            if (byte >= g_len && col == 0 && r > cur_row - scroll_row) break;
        }

        /* Blinking caret. */
        if (has_focus && ((caret_phase / 12) & 1) == 0) {
            int cy = text_y0 + (cur_row - scroll_row) * LINE_H;
            int cx = GRID_X + cur_col * CELL_W;
            if (cur_row >= scroll_row && cur_row < scroll_row + max_rows
                && cur_col <= max_cols) {
                wm_fill_rect(&win, cx, cy, CELL_W, LINE_H - 2, 0xFFFFFFu);
            }
        }
        caret_phase++;

        /* Footer status bar. */
        int fy = WIN_H - FOOTER_H + 2;
        wm_fill_rect(&win, 0, WIN_H - FOOTER_H, WIN_W, FOOTER_H,
                     0x202830u);
        int fx = draw_str(&sctx, 6, fy, g_path, 0xE0E0E0u);
        fx += 4;
        if (g_dirty) fx = draw_str(&sctx, fx, fy, "[*]", 0xE0E030u);
        fx += 8;
        char info[40];
        int n = 0;
        const char *p = "L"; while (*p && n < 39) info[n++] = *p++;
        n += dec(info + n, (int)sizeof(info) - n, (unsigned)cur_row + 1);
        if (n < 39) info[n++] = ':';
        n += dec(info + n, (int)sizeof(info) - n, (unsigned)cur_col + 1);
        if (n < 39) info[n++] = ' ';
        if (n < 39) info[n++] = 'B';
        n += dec(info + n, (int)sizeof(info) - n, (unsigned)g_len);
        info[n] = 0;
        draw_str(&sctx, fx, fy, info, 0x80C080u);

        wm_present(&win);
        sys_sleep_ms(33);
    }

    wm_close(&win);
    printf("wmedit: closed (len=%d, dirty=%d)\n", g_len, g_dirty);
    return 0;
}
