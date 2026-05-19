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
/* Session 140 — drag selection.  g_sel_anchor is the byte offset
 * where the most recent mouse-press landed (or -1 if no selection
 * is in progress).  The selection range is [min(anchor, cur),
 * max(anchor, cur)); empty when anchor == cur.  g_drag is 1 while
 * the left mouse button is held down (between PRESS and RELEASE)
 * so MOUSE_MOVE knows whether to extend the selection. */
static int   g_sel_anchor = -1;
static int   g_drag;
static int   g_scroll_row;   /* hoisted out of main() so byte_at_xy
                              * can use it from event handlers */

/* Session 152 — incremental search.  Ctrl-F opens a search bar
 * at the bottom; typing builds g_search_pat; matches in the
 * viewport are highlighted yellow.  Enter jumps the cursor to
 * the next match (wrapping); Esc closes search.  We keep the
 * pattern + match-state across frames so the highlight survives
 * scrolling. */
#define SEARCH_PAT_MAX 48
static int  g_search_mode;
static char g_search_pat[SEARCH_PAT_MAX];
static int  g_search_len;
/* Find the next match at or after `from`, wrapping to 0 if no
 * match in [from, len].  Returns the byte offset of the match
 * or -1 if g_search_pat doesn't occur in g_buf. */
static int find_match(int from) {
    if (g_search_len <= 0 || g_search_len > g_len) return -1;
    int last = g_len - g_search_len;
    for (int i = from; i <= last; i++) {
        int ok = 1;
        for (int j = 0; j < g_search_len; j++) {
            if (g_buf[i + j] != g_search_pat[j]) { ok = 0; break; }
        }
        if (ok) return i;
    }
    for (int i = 0; i < from && i <= last; i++) {
        int ok = 1;
        for (int j = 0; j < g_search_len; j++) {
            if (g_buf[i + j] != g_search_pat[j]) { ok = 0; break; }
        }
        if (ok) return i;
    }
    return -1;
}

static int sel_lo(void) {
    return g_sel_anchor < g_cur ? g_sel_anchor : g_cur;
}
static int sel_hi(void) {
    return g_sel_anchor < g_cur ? g_cur : g_sel_anchor;
}
static int sel_active(void) {
    return g_sel_anchor >= 0 && g_sel_anchor != g_cur;
}
static void sel_clear(void) { g_sel_anchor = -1; }

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
    /* Session 153 — load_file runs at startup before any edits,
     * so g_undo_count is already 0 by BSS init. */
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

/* Session 140 — drop the active selection from the buffer.  Cursor
 * lands at the deleted range's lower bound; selection collapses. */
static void sel_delete(void) {
    if (!sel_active()) return;
    int lo = sel_lo();
    int hi = sel_hi();
    int n  = hi - lo;
    for (int i = lo; i < g_len - n; i++) g_buf[i] = g_buf[i + n];
    g_len -= n;
    g_cur = lo;
    g_sel_anchor = -1;
    g_dirty = 1;
}

/* Session 153 — undo stack.  One entry per edit, with light
 * coalescing: consecutive forward-typing or contiguous-backspace
 * runs collapse into a single Ctrl-Z step.  Per-entry text cap is
 * 40 bytes, so very large sel_delete()s only partially restore
 * (cursor still lands at the right offset; missing bytes lost).
 *
 * No redo (Ctrl-Y) yet — once you type after undoing, the popped
 * history is gone.  Same convention as a typical first-pass
 * editor undo. */
#define UNDO_MAX 64
#define UNDO_PIECE_MAX 40

struct undo_entry {
    int  kind;     /* 1 = insert (delete to undo), 2 = delete (re-insert to undo) */
    int  offset;
    int  len;
    char text[UNDO_PIECE_MAX];
};
static struct undo_entry g_undo[UNDO_MAX];
static int g_undo_count;

static void undo_push(int kind, int offset, int len, const char *text) {
    if (g_undo_count > 0) {
        struct undo_entry *prev = &g_undo[g_undo_count - 1];
        /* Type-forward coalesce: prev was insert ending exactly at
         * the current offset, and there's room. */
        if (kind == 1 && prev->kind == 1
            && offset == prev->offset + prev->len
            && prev->len + len <= UNDO_PIECE_MAX) {
            prev->len += len;
            return;
        }
        /* Backspace coalesce: prev was a delete whose offset is one
         * past our deletion's offset (so we're erasing leftward into
         * the same run). */
        if (kind == 2 && prev->kind == 2 && len == 1 && text
            && offset + 1 == prev->offset
            && prev->len + 1 <= UNDO_PIECE_MAX) {
            for (int i = prev->len - 1; i >= 0; i--)
                prev->text[i + 1] = prev->text[i];
            prev->text[0] = text[0];
            prev->offset = offset;
            prev->len++;
            return;
        }
    }
    /* Push a fresh entry; drop oldest if at cap. */
    if (g_undo_count >= UNDO_MAX) {
        for (int i = 0; i < UNDO_MAX - 1; i++)
            g_undo[i] = g_undo[i + 1];
        g_undo_count = UNDO_MAX - 1;
    }
    struct undo_entry *e = &g_undo[g_undo_count++];
    e->kind = kind;
    e->offset = offset;
    e->len = len;
    if (kind == 2 && text) {
        int n = len > UNDO_PIECE_MAX ? UNDO_PIECE_MAX : len;
        for (int i = 0; i < n; i++) e->text[i] = text[i];
    }
}

static void undo_pop(void) {
    if (g_undo_count == 0) return;
    struct undo_entry *e = &g_undo[--g_undo_count];
    if (e->kind == 1) {
        /* Insert was: bytes added at [offset, offset+len).  Remove
         * them; place cursor at offset. */
        for (int i = e->offset; i + e->len < g_len; i++)
            g_buf[i] = g_buf[i + e->len];
        g_len -= e->len;
        g_cur = e->offset;
    } else if (e->kind == 2) {
        /* Delete removed e->text from offset.  Re-insert and place
         * cursor after the restored block. */
        int len = e->len > UNDO_PIECE_MAX ? UNDO_PIECE_MAX : e->len;
        if (g_len + len < BUF_MAX) {
            for (int i = g_len + len - 1; i >= e->offset + len; i--)
                g_buf[i] = g_buf[i - len];
            for (int i = 0; i < len; i++)
                g_buf[e->offset + i] = e->text[i];
            g_len += len;
            g_cur = e->offset + len;
        }
    }
    sel_clear();
    g_dirty = 1;
}

/* Undoable wrappers around the bare buf_/sel_delete helpers.
 * These record the inverse operation BEFORE mutating g_buf so an
 * Ctrl-Z replays the original state exactly. */
static void buf_insert_record(char c) {
    if (g_len >= BUF_MAX - 1) return;
    undo_push(1, g_cur, 1, NULL);
    buf_insert(c);
}
static void buf_delete_record(void) {
    if (g_cur == 0) return;
    char snap = g_buf[g_cur - 1];
    undo_push(2, g_cur - 1, 1, &snap);
    buf_delete();
}
static void sel_delete_record(void) {
    if (!sel_active()) return;
    int lo = sel_lo();
    int hi = sel_hi();
    int len = hi - lo;
    int snap_len = len > UNDO_PIECE_MAX ? UNDO_PIECE_MAX : len;
    char snap[UNDO_PIECE_MAX];
    for (int i = 0; i < snap_len; i++) snap[i] = g_buf[lo + i];
    undo_push(2, lo, snap_len, snap);
    sel_delete();
}

/* Compute the byte offset at the on-surface coordinates (ex, ey).
 * Used by both MOUSE_PRESS (cursor place) and MOUSE_MOVE (extend
 * selection during drag). */
static int byte_at_xy(int ex, int ey) {
    int rel_y = ey - HDR_H - 4;
    int rel_x = ex - GRID_X;
    if (rel_y < 0) rel_y = 0;
    if (rel_x < 0) rel_x = 0;
    int row = g_scroll_row + rel_y / LINE_H;
    int col = rel_x / CELL_W;
    int start = row_start_off(row);
    int end   = row_start_off(row + 1);
    if (end > start && g_buf[end - 1] == '\n') end--;
    if (col > end - start) col = end - start;
    return start + col;
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
    g_scroll_row = 0;

    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_CLOSE:   closed = 1; break;
                case WM_EV_KEY: {
                    unsigned int k = ev.keycode;
                    /* Session 152 — search-mode key dispatch.  When
                     * Ctrl-F has opened the search bar, all keystrokes
                     * go into g_search_pat instead of g_buf.  Esc
                     * closes the bar; Enter jumps to the next match;
                     * Backspace shortens the pattern.  Cursor + buffer
                     * are untouched (search is read-only navigation). */
                    if (g_search_mode) {
                        if (k == 27) {              /* Esc — close */
                            g_search_mode = 0;
                            break;
                        }
                        if (k == '\r' || k == '\n') {
                            int m = find_match(g_cur + 1);
                            if (m >= 0) g_cur = m;
                            break;
                        }
                        if (k == 0x08 || k == 0x7F) {
                            if (g_search_len > 0) g_search_len--;
                            g_search_pat[g_search_len] = 0;
                            /* Re-find from current cursor (or start). */
                            int m = find_match(g_cur);
                            if (m >= 0) g_cur = m;
                            break;
                        }
                        if (k >= 0x20 && k <= 0x7E
                            && g_search_len < SEARCH_PAT_MAX - 1) {
                            g_search_pat[g_search_len++] = (char)k;
                            g_search_pat[g_search_len] = 0;
                            int m = find_match(g_cur);
                            if (m >= 0) g_cur = m;
                            break;
                        }
                        /* Anything else ignored while searching. */
                        break;
                    }
                    /* ANSI CSI: ESC '[' <final>. */
                    if (esc_state == 1) {
                        if (k == '[') { esc_state = 2; break; }
                        esc_state = 0;
                        break;
                    }
                    if (esc_state == 2) {
                        esc_state = 0;
                        if (k == 'A' || k == 'B' || k == 'C' || k == 'D') {
                            cursor_move((char)k);
                            sel_clear();
                        }
                        break;
                    }
                    if (k == 27) { esc_state = 1; break; }
                    if (k == 0x06) {                  /* Ctrl-F — open search */
                        g_search_mode = 1;
                        g_search_len = 0;
                        g_search_pat[0] = 0;
                        sel_clear();
                        break;
                    }
                    if (k == 0x1A) {                  /* Ctrl-Z — undo */
                        undo_pop();
                        break;
                    }
                    if (k == 0x13) {              /* Ctrl+S — save */
                        int rc = save_file();
                        /* Session 143 — toast feedback. */
                        char tn[80];
                        int p = 0;
                        const char *m = (rc == 0) ? "saved " : "save failed: ";
                        while (*m && p < (int)sizeof(tn) - 1) tn[p++] = *m++;
                        for (int i = 0; g_path[i] && p < (int)sizeof(tn) - 1; i++)
                            tn[p++] = g_path[i];
                        if (rc == 0) {
                            const char *q = " (";
                            while (*q && p < (int)sizeof(tn) - 1) tn[p++] = *q++;
                            p += dec(tn + p, (int)sizeof(tn) - p, (unsigned)g_len);
                            const char *r = " B)";
                            while (*r && p < (int)sizeof(tn) - 1) tn[p++] = *r++;
                        }
                        tn[p] = 0;
                        wm_notify(tn);
                    } else if (k == 0x11) {       /* Ctrl+Q — quit */
                        closed = 1;
                    } else if (k == 0x03) {       /* Ctrl+C — copy */
                        int copied;
                        if (sel_active()) {
                            copied = sel_hi() - sel_lo();
                            wm_clipboard_set(g_buf + sel_lo(), copied);
                        } else {
                            copied = g_len;
                            wm_clipboard_set(g_buf, g_len);
                        }
                        /* Session 143 — toast on copy. */
                        char tn[64];
                        int p = 0;
                        const char *m = "copied ";
                        while (*m && p < (int)sizeof(tn) - 1) tn[p++] = *m++;
                        p += dec(tn + p, (int)sizeof(tn) - p, (unsigned)copied);
                        const char *r = " B";
                        while (*r && p < (int)sizeof(tn) - 1) tn[p++] = *r++;
                        tn[p] = 0;
                        wm_notify(tn);
                    } else if (k == 0x18) {       /* Ctrl+X — cut */
                        if (sel_active()) {
                            wm_clipboard_set(g_buf + sel_lo(),
                                             sel_hi() - sel_lo());
                            sel_delete_record();
                        }
                    } else if (k == 0x16) {       /* Ctrl+V — paste */
                        if (sel_active()) sel_delete_record();
                        char pb[BUF_MAX];
                        int pn = wm_clipboard_get(pb, (int)sizeof(pb));
                        if (pn > 0) {
                            if (pn > BUF_MAX - g_len - 1) pn = BUF_MAX - g_len - 1;
                            for (int i = 0; i < pn; i++) buf_insert_record(pb[i]);
                        }
                    } else if (k == 0x08 || k == 0x7F) {
                        if (sel_active()) sel_delete_record();
                        else              buf_delete_record();
                    } else if (k == '\r' || k == '\n') {
                        if (sel_active()) sel_delete_record();
                        buf_insert_record('\n');
                    } else if (k == '\t') {
                        if (sel_active()) sel_delete_record();
                        for (int i = 0; i < 4; i++) buf_insert_record(' ');
                    } else if (k >= 0x20 && k <= 0x7E) {
                        if (sel_active()) sel_delete_record();
                        buf_insert_record((char)k);
                    }
                    break;
                }
                case WM_EV_MOUSE_PRESS: {
                    /* Click in the text region: position cursor and
                     * start a (potential) drag selection.  Anchor =
                     * cursor; if the user drags, MOUSE_MOVE extends
                     * the selection.  On RELEASE with no movement
                     * the selection collapses (anchor cleared). */
                    int off = byte_at_xy(ev.x, ev.y);
                    g_cur = off;
                    g_sel_anchor = off;
                    g_drag = 1;
                    break;
                }
                case WM_EV_MOUSE_MOVE: {
                    if (g_drag) {
                        g_cur = byte_at_xy(ev.x, ev.y);
                    }
                    break;
                }
                case WM_EV_MOUSE_RELEASE: {
                    g_drag = 0;
                    if (g_cur == g_sel_anchor) g_sel_anchor = -1;
                    break;
                }
                default: break;
            }
        }

        /* Scroll so the cursor stays in view. */
        int cur_row, cur_col;
        cursor_rowcol(g_cur, &cur_row, &cur_col);
        if (cur_row < g_scroll_row) g_scroll_row = cur_row;
        if (cur_row >= g_scroll_row + max_rows)
            g_scroll_row = cur_row - max_rows + 1;
        if (g_scroll_row < 0) g_scroll_row = 0;

        /* Repaint. */
        wm_clear(&win, has_focus ? 0x0A0A14u : 0x141414u);

        /* Title bar. */
        wm_fill_rect(&win, 0, 0, WIN_W, HDR_H,
                     has_focus ? 0x4080E0u : 0x404040u);
        gfx_text(&sctx, 6, 5,
                 has_focus ? "wmedit  Ctrl-S save  Ctrl-Q quit"
                           : "wmedit  (click to focus)",
                 GFX_WHITE, GFX_TRANSPARENT);

        /* Session 152 — find all visible matches before painting so
         * the body loop can yellow-highlight each match span. */
        int matches[64];
        int n_matches = 0;
        if (g_search_mode && g_search_len > 0
            && g_search_len <= g_len) {
            int last = g_len - g_search_len;
            int i = 0;
            while (i <= last && n_matches < 64) {
                int ok = 1;
                for (int j = 0; j < g_search_len; j++) {
                    if (g_buf[i + j] != g_search_pat[j]) { ok = 0; break; }
                }
                if (ok) {
                    matches[n_matches++] = i;
                    i += g_search_len;
                } else {
                    i++;
                }
            }
        }

        /* Text body. */
        int text_y0 = HDR_H + 4;
        int byte = row_start_off(g_scroll_row);
        /* Session 140 — selection bounds (one byte half-open). */
        int slo = sel_active() ? sel_lo() : -1;
        int shi = sel_active() ? sel_hi() : -1;
        for (int r = 0; r < max_rows; r++) {
            int y = text_y0 + r * LINE_H;
            int col = 0;
            while (byte < g_len && g_buf[byte] != '\n') {
                if (col < max_cols) {
                    /* Selection highlight behind the glyph. */
                    if (byte >= slo && byte < shi) {
                        wm_fill_rect(&win, GRID_X + col * CELL_W, y,
                                     CELL_W, LINE_H, 0x305078u);
                    }
                    /* Session 152 — search-match highlight (mustard
                     * yellow), painted under glyph + over selection. */
                    for (int m = 0; m < n_matches; m++) {
                        if (byte >= matches[m]
                            && byte < matches[m] + g_search_len) {
                            wm_fill_rect(&win, GRID_X + col * CELL_W, y,
                                         CELL_W, LINE_H, 0x807030u);
                            break;
                        }
                    }
                    gfx_glyph(&sctx, GRID_X + col * CELL_W, y,
                              g_buf[byte], 0xC0E0C0u, GFX_TRANSPARENT);
                }
                col++;
                byte++;
            }
            /* Newline included in the selection range gets a one-
             * cell highlight at end-of-line so cross-row drags read
             * as continuous. */
            if (byte < g_len && g_buf[byte] == '\n') {
                if (col < max_cols && byte >= slo && byte < shi) {
                    wm_fill_rect(&win, GRID_X + col * CELL_W, y,
                                 CELL_W, LINE_H, 0x305078u);
                }
                byte++;
            }
            if (byte >= g_len && col == 0 && r > cur_row - g_scroll_row) break;
        }

        /* Blinking caret — 2-pixel vertical line at the cursor.  No
         * caret while the selection is non-empty (the highlight
         * already shows where the cursor end is). */
        if (has_focus && !sel_active() && ((caret_phase / 12) & 1) == 0) {
            int cy = text_y0 + (cur_row - g_scroll_row) * LINE_H;
            int cx = GRID_X + cur_col * CELL_W;
            if (cur_row >= g_scroll_row && cur_row < g_scroll_row + max_rows
                && cur_col <= max_cols) {
                wm_fill_rect(&win, cx, cy, 2, LINE_H - 1, 0xFFFFFFu);
            }
        }
        caret_phase++;

        /* Footer status bar.  Session 152 — in search mode the
         * footer becomes "Find: <pat>_  Esc:close  Enter:next" with
         * a distinct yellow-tinted background so the mode is
         * unmistakable. */
        int fy = WIN_H - FOOTER_H + 2;
        if (g_search_mode) {
            wm_fill_rect(&win, 0, WIN_H - FOOTER_H, WIN_W, FOOTER_H,
                         0x403820u);   /* dark mustard */
            int fx = draw_str(&sctx, 6, fy, "Find: ", 0xFFFFFFu);
            fx = draw_str(&sctx, fx, fy, g_search_pat, 0xFFE080u);
            /* Blinking '_' cursor at end of pattern. */
            if (((caret_phase / 12) & 1) == 0) {
                draw_str(&sctx, fx, fy, "_", 0xFFFFFFu);
            }
            draw_str(&sctx, WIN_W - 200, fy,
                     "Esc:close  Enter:next", 0xC0C0A0u);
        } else {
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
        }

        wm_present(&win);
        sys_sleep_ms(33);
    }

    wm_close(&win);
    printf("wmedit: closed (len=%d, dirty=%d)\n", g_len, g_dirty);
    return 0;
}
