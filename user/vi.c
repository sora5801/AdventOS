/*
 * vi — a modal text editor, vi(1)-ish, for AdventOS.
 *
 * The existing user/ed.c is an ed(1)-style line editor — fine for
 * `s/foo/bar/` style scripted edits, painful for "open a file and
 * type". This is the screen-oriented counterpart: normal mode for
 * movement + commands, insert mode for typing, command-line mode
 * for `:w`, `:q`, search, etc.
 *
 * Implementation choices:
 *
 *   - Buffer model: array of N lines, each a heap-allocated char *.
 *     Simple, O(1) line access, O(n) line insertion. Plenty for the
 *     2 KiB files we edit; a real editor would use a gap buffer or
 *     a piece table to avoid the linear shift on inserts.
 *
 *   - Screen: 80x25 hardcoded (matches VGA text-mode + fbcon's 8x8
 *     glyph grid on the demo's 1024x768 framebuffer). One status line
 *     at the bottom; 24 lines of buffer above.
 *
 *   - Redraw: full-screen rewrite on every key. We have sys_tty_clear /
 *     sys_tty_cursor / sys_tty_clear_eol now (session 46), so a full
 *     redraw is two screenfulls of bytes plus a handful of cursor-
 *     positioning syscalls. Cheap enough at 80x24 = 1920 chars.
 *
 *   - Input: TTY_RAW mode, sys_read(0, byte, 1) for one byte at a
 *     time. Arrow keys are NOT delivered by AdventOS's keyboard
 *     driver (no ANSI sequences) — hjkl is the only navigation,
 *     which is fully vi-spirited anyway.
 *
 * Command coverage (intentionally small but recognizable):
 *
 *   Movement   h j k l 0 $ gg G w b
 *   Insert     i a I A o O      ESC to leave
 *   Delete     x dd
 *   Yank       yy
 *   Paste      p P
 *   Search     /pat then n / N
 *   Cmd-line   :w :q :q! :wq :NN (goto line N)
 */

#include "libuser.h"

#define MAX_LINES   2048
#define MAX_LINE    1024
#define SCREEN_COLS 80
#define SCREEN_ROWS 25
#define BODY_ROWS   (SCREEN_ROWS - 1)     /* one row reserved for status */

/* ---- Buffer ---------------------------------------------------- */

struct line {
    char *text;    /* malloc'd; not NUL-terminated, len tracks length */
    int   len;
    int   cap;
};

static struct line g_lines[MAX_LINES];
static int         g_n_lines;

static char g_filename[64];
static int  g_modified;

/* Cursor in buffer space; view scrolls when cursor leaves the window. */
static int g_cr, g_cc;
static int g_top_row;

/* Modes. */
enum { MODE_NORMAL = 0, MODE_INSERT = 1, MODE_COMMAND = 2 };
static int g_mode;

/* Pending "d" / "g" / "y" prefix for two-key ops (dd, gg, yy). */
static char g_pending;

/* Status / message at the bottom row. Replaced each redraw. */
static char g_status[80];

/* `:`/`/` line buffer. */
static char g_cmd_buf[80];
static int  g_cmd_len;
static char g_cmd_lead;          /* ':' or '/' */

/* Yank register. Holds either a partial line (yank_is_line=0) or
 * a whole line (yank_is_line=1, p/P inserts above/below as a new
 * line rather than into the current one). */
static char g_yank_buf[MAX_LINE];
static int  g_yank_len;
static int  g_yank_is_line;

/* Last search pattern, persists across n / N. */
static char g_search_pat[80];
static int  g_search_len;

static int  g_quit;

/* ---- Tiny utilities ------------------------------------------- */

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}
static void my_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int my_memcmp(const void *a, const void *b, int n) {
    const unsigned char *p = a, *q = b;
    for (int i = 0; i < n; i++) if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}
static void out_str(const char *s) {
    int n = my_strlen(s);
    sys_write(1, s, n);
}
static void out_char(char c) { sys_write(1, &c, 1); }
static void out_int(int v) {
    char b[12]; int n = 0;
    if (v < 0) { out_char('-'); v = -v; }
    if (v == 0) b[n++] = '0';
    else while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) out_char(b[n]);
}

/* ---- Line storage --------------------------------------------- */

static void line_grow(struct line *L, int min_cap) {
    if (L->cap >= min_cap) return;
    int new_cap = L->cap ? L->cap : 80;
    while (new_cap < min_cap) new_cap *= 2;
    if (new_cap > MAX_LINE) new_cap = MAX_LINE;
    char *nb = malloc(new_cap);
    if (!nb) return;
    for (int i = 0; i < L->len; i++) nb[i] = L->text[i];
    if (L->text) free(L->text);
    L->text = nb;
    L->cap = new_cap;
}

static void buffer_init(void) {
    g_n_lines = 1;
    g_lines[0].text = malloc(80);
    g_lines[0].text[0] = 0;
    g_lines[0].len = 0;
    g_lines[0].cap = 80;
}

/* Insert a fresh empty line at index `idx` — shifts later lines down. */
static void buffer_insert_line(int idx) {
    if (g_n_lines >= MAX_LINES) return;
    for (int i = g_n_lines; i > idx; i--) g_lines[i] = g_lines[i - 1];
    g_lines[idx].text = malloc(80);
    g_lines[idx].text[0] = 0;
    g_lines[idx].len = 0;
    g_lines[idx].cap = 80;
    g_n_lines++;
}

static void buffer_delete_line(int idx) {
    if (g_n_lines <= 1) {
        /* Last line — empty it rather than removing. */
        g_lines[idx].len = 0;
        return;
    }
    if (g_lines[idx].text) free(g_lines[idx].text);
    for (int i = idx; i < g_n_lines - 1; i++) g_lines[i] = g_lines[i + 1];
    g_n_lines--;
}

static void line_insert_char(int idx, int col, char c) {
    struct line *L = &g_lines[idx];
    line_grow(L, L->len + 2);
    for (int i = L->len; i > col; i--) L->text[i] = L->text[i - 1];
    L->text[col] = c;
    L->len++;
}

static void line_delete_char(int idx, int col) {
    struct line *L = &g_lines[idx];
    if (col >= L->len) return;
    for (int i = col; i < L->len - 1; i++) L->text[i] = L->text[i + 1];
    L->len--;
}

/* Split current line at column `col`: bytes 0..col-1 stay on current
 * line, bytes col..len-1 move to a NEW line inserted after. Used by
 * Enter in insert mode. */
static void line_split(int idx, int col) {
    if (g_n_lines >= MAX_LINES) return;
    struct line *L = &g_lines[idx];
    int tail_len = L->len - col;
    if (tail_len < 0) tail_len = 0;
    buffer_insert_line(idx + 1);
    struct line *N = &g_lines[idx + 1];
    line_grow(N, tail_len + 1);
    for (int i = 0; i < tail_len; i++) N->text[i] = L->text[col + i];
    N->len = tail_len;
    L->len = col;
}

/* Join line `idx` with the line after it. */
static void line_join(int idx) {
    if (idx + 1 >= g_n_lines) return;
    struct line *L = &g_lines[idx];
    struct line *N = &g_lines[idx + 1];
    line_grow(L, L->len + N->len + 1);
    for (int i = 0; i < N->len; i++) L->text[L->len + i] = N->text[i];
    L->len += N->len;
    buffer_delete_line(idx + 1);
}

/* ---- File I/O -------------------------------------------------- */

static void file_load(const char *path) {
    int fd = sys_open(path);
    if (fd < 0) {
        /* New file — buffer is already one empty line from init. */
        return;
    }
    g_n_lines = 0;
    char chunk[512];
    /* Append into the current line; on '\n' close it and start the next. */
    int cur_idx = -1;
    int n;
    while ((n = sys_read(fd, chunk, sizeof(chunk))) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = chunk[i];
            if (cur_idx < 0) {
                if (g_n_lines >= MAX_LINES) break;
                cur_idx = g_n_lines++;
                g_lines[cur_idx].text = malloc(80);
                g_lines[cur_idx].text[0] = 0;
                g_lines[cur_idx].len = 0;
                g_lines[cur_idx].cap = 80;
            }
            if (ch == '\n') {
                cur_idx = -1;
            } else if (ch != '\r') {
                struct line *L = &g_lines[cur_idx];
                if (L->len < MAX_LINE - 1) {
                    line_grow(L, L->len + 2);
                    L->text[L->len++] = ch;
                }
            }
        }
    }
    sys_close(fd);
    if (g_n_lines == 0) {
        /* Empty file → one empty line. */
        buffer_init();
    }
}

static int file_save(void) {
    /* Compute total size, then write once via sys_fs_write (atomic
     * file overwrite — same pattern as ed). */
    int total = 0;
    for (int i = 0; i < g_n_lines; i++) total += g_lines[i].len + 1;
    char *buf = malloc(total + 1);
    if (!buf) return -1;
    int o = 0;
    for (int i = 0; i < g_n_lines; i++) {
        for (int j = 0; j < g_lines[i].len; j++) buf[o++] = g_lines[i].text[j];
        buf[o++] = '\n';
    }
    int rc = sys_fs_write(g_filename, buf, (unsigned)o);
    free(buf);
    if (rc == 0) g_modified = 0;
    return rc;
}

/* ---- Search ---------------------------------------------------- */

/* Find the next occurrence of g_search_pat starting at (start_row, start_col).
 * Returns 1 + sets g_cr/g_cc on hit, 0 on miss. Wraps. */
static int search_next(int start_row, int start_col, int forward) {
    if (g_search_len == 0) return 0;
    int rows_tried = 0;
    int r = start_row;
    int c = start_col;
    while (rows_tried <= g_n_lines) {
        struct line *L = &g_lines[r];
        if (forward) {
            for (int i = c; i + g_search_len <= L->len; i++) {
                if (my_memcmp(L->text + i, g_search_pat, g_search_len) == 0) {
                    g_cr = r; g_cc = i;
                    return 1;
                }
            }
            r++;
            if (r >= g_n_lines) r = 0;
            c = 0;
        } else {
            for (int i = c; i >= 0; i--) {
                if (i + g_search_len <= L->len &&
                    my_memcmp(L->text + i, g_search_pat, g_search_len) == 0)
                {
                    g_cr = r; g_cc = i;
                    return 1;
                }
            }
            r--;
            if (r < 0) r = g_n_lines - 1;
            c = g_lines[r].len - 1;
        }
        rows_tried++;
    }
    return 0;
}

/* ---- Redraw ---------------------------------------------------- */

static void redraw(void) {
    /* Keep cursor row inside the visible window (single-line scroll). */
    if (g_cr < g_top_row) g_top_row = g_cr;
    if (g_cr >= g_top_row + BODY_ROWS) g_top_row = g_cr - BODY_ROWS + 1;
    if (g_top_row < 0) g_top_row = 0;

    /* Body. Each row: position to (row, 0), print up to 80 chars from
     * the buffer (or "~" if past end of file), clear residual. */
    for (int sr = 0; sr < BODY_ROWS; sr++) {
        sys_tty_cursor(sr, 0);
        int br = g_top_row + sr;
        if (br < g_n_lines) {
            int n = g_lines[br].len;
            if (n > SCREEN_COLS) n = SCREEN_COLS;
            if (n > 0) sys_write(1, g_lines[br].text, n);
        } else {
            out_char('~');
        }
        sys_tty_clear_eol();
    }

    /* Status line. Either the current `:` / `/` buffer, or a
     * mode + position summary. */
    sys_tty_cursor(BODY_ROWS, 0);
    if (g_mode == MODE_COMMAND) {
        out_char(g_cmd_lead);
        sys_write(1, g_cmd_buf, g_cmd_len);
    } else {
        const char *modestr = (g_mode == MODE_INSERT) ? "-- INSERT --" : "";
        if (g_status[0]) {
            out_str(g_status);
        } else {
            out_str(modestr);
        }
        /* Right-aligned position. */
        int used = my_strlen(g_status[0] ? g_status : modestr);
        char info[40]; int n = 0;
        info[n++] = ' ';
        info[n++] = '['; int r = 0;
        /* "name modified row,col" */
        for (int i = 0; g_filename[i] && r < 20; i++, r++) info[n++] = g_filename[i];
        if (g_modified) { info[n++] = ' '; info[n++] = '*'; }
        info[n++] = ']';
        info[n++] = ' ';
        int row1 = g_cr + 1, col1 = g_cc + 1;
        char rb[8]; int rn = 0;
        if (row1 == 0) rb[rn++] = '0';
        while (row1) { rb[rn++] = (char)('0' + row1 % 10); row1 /= 10; }
        while (rn--) info[n++] = rb[rn];
        info[n++] = ',';
        char cb[8]; int cn = 0;
        if (col1 == 0) cb[cn++] = '0';
        while (col1) { cb[cn++] = (char)('0' + col1 % 10); col1 /= 10; }
        while (cn--) info[n++] = cb[cn];
        /* Pad to right edge. */
        int pad = SCREEN_COLS - used - n;
        while (pad-- > 0) out_char(' ');
        sys_write(1, info, n);
    }
    sys_tty_clear_eol();

    /* Place the cursor at the editing point (or the end of the
     * command-line in command mode). */
    if (g_mode == MODE_COMMAND) {
        sys_tty_cursor(BODY_ROWS, g_cmd_len + 1);
    } else {
        int row = g_cr - g_top_row;
        int col = g_cc;
        if (col > SCREEN_COLS - 1) col = SCREEN_COLS - 1;
        sys_tty_cursor(row, col);
    }

    /* One-shot status messages clear after one redraw. */
    g_status[0] = 0;
}

static void set_status(const char *s) {
    my_strncpy(g_status, s, (int)sizeof(g_status));
}

/* ---- Cursor helpers ------------------------------------------- */

static void clamp_cursor(void) {
    if (g_cr < 0) g_cr = 0;
    if (g_cr >= g_n_lines) g_cr = g_n_lines - 1;
    if (g_cc < 0) g_cc = 0;
    int max = g_lines[g_cr].len - (g_mode == MODE_INSERT ? 0 : 1);
    if (max < 0) max = 0;
    if (g_cc > max) g_cc = max;
}

/* ---- Command handlers ----------------------------------------- */

static void run_colon_command(const char *cmd, int len) {
    if (len == 0) return;
    /* :NN — goto line. */
    if (cmd[0] >= '0' && cmd[0] <= '9') {
        int v = 0;
        for (int i = 0; i < len; i++) {
            if (cmd[i] < '0' || cmd[i] > '9') return;
            v = v * 10 + (cmd[i] - '0');
        }
        if (v < 1) v = 1;
        if (v > g_n_lines) v = g_n_lines;
        g_cr = v - 1;
        g_cc = 0;
        return;
    }
    /* :w / :q / :wq / :q! */
    int want_w = 0, want_q = 0, force = 0;
    for (int i = 0; i < len; i++) {
        if (cmd[i] == 'w') want_w = 1;
        else if (cmd[i] == 'q') want_q = 1;
        else if (cmd[i] == '!') force = 1;
    }
    if (want_w) {
        if (file_save() == 0) {
            char tmp[80];
            int n = 0;
            const char *p = "\"";
            while (*p) tmp[n++] = *p++;
            for (int i = 0; g_filename[i] && n < 60; i++) tmp[n++] = g_filename[i];
            const char *suf = "\" written";
            while (*suf) tmp[n++] = *suf++;
            tmp[n] = 0;
            set_status(tmp);
        } else {
            set_status("write failed");
        }
    }
    if (want_q) {
        if (g_modified && !force && !want_w) {
            set_status("E37: unsaved changes (use :q!)");
            return;
        }
        g_quit = 1;
    }
}

static void run_search(const char *pat, int len) {
    if (len == 0) return;
    my_strncpy(g_search_pat, pat, (int)sizeof(g_search_pat));
    g_search_len = len;
    /* Start searching from one past the cursor. */
    int sr = g_cr, sc = g_cc + 1;
    if (sc > g_lines[sr].len) { sc = 0; sr++; if (sr >= g_n_lines) sr = 0; }
    if (!search_next(sr, sc, 1)) set_status("pattern not found");
}

/* ---- Mode handlers --------------------------------------------- */

static void handle_normal(int c) {
    /* Two-key sequences: 'd' + 'd' → dd, 'g' + 'g' → gg, 'y' + 'y' → yy. */
    if (g_pending) {
        char p = g_pending;
        g_pending = 0;
        if (p == 'd' && c == 'd') {
            /* dd — also acts as the yank source. */
            struct line *L = &g_lines[g_cr];
            int n = L->len; if (n > MAX_LINE - 1) n = MAX_LINE - 1;
            for (int i = 0; i < n; i++) g_yank_buf[i] = L->text[i];
            g_yank_len = n;
            g_yank_is_line = 1;
            buffer_delete_line(g_cr);
            if (g_cr >= g_n_lines) g_cr = g_n_lines - 1;
            g_cc = 0;
            g_modified = 1;
            return;
        }
        if (p == 'g' && c == 'g') {
            g_cr = 0; g_cc = 0;
            return;
        }
        if (p == 'y' && c == 'y') {
            struct line *L = &g_lines[g_cr];
            int n = L->len; if (n > MAX_LINE - 1) n = MAX_LINE - 1;
            for (int i = 0; i < n; i++) g_yank_buf[i] = L->text[i];
            g_yank_len = n;
            g_yank_is_line = 1;
            set_status("1 line yanked");
            return;
        }
        /* Unknown two-key seq — fall through and reinterpret c. */
    }

    switch (c) {
        case 'h': g_cc--; break;
        case 'l': g_cc++; break;
        case 'j': g_cr++; break;
        case 'k': g_cr--; break;
        case '0': g_cc = 0; break;
        case '$': g_cc = g_lines[g_cr].len ? g_lines[g_cr].len - 1 : 0; break;
        case 'w': {
            /* Skip alphanumerics, then skip whitespace, land on next word. */
            struct line *L = &g_lines[g_cr];
            while (g_cc < L->len &&
                   ((L->text[g_cc] >= 'a' && L->text[g_cc] <= 'z') ||
                    (L->text[g_cc] >= 'A' && L->text[g_cc] <= 'Z') ||
                    (L->text[g_cc] >= '0' && L->text[g_cc] <= '9') ||
                    L->text[g_cc] == '_')) g_cc++;
            while (g_cc < L->len && L->text[g_cc] == ' ') g_cc++;
            if (g_cc >= L->len && g_cr < g_n_lines - 1) { g_cr++; g_cc = 0; }
            break;
        }
        case 'b': {
            struct line *L = &g_lines[g_cr];
            if (g_cc == 0 && g_cr > 0) {
                g_cr--; g_cc = g_lines[g_cr].len ? g_lines[g_cr].len - 1 : 0;
            } else {
                g_cc--;
                while (g_cc > 0 && L->text[g_cc] == ' ') g_cc--;
                while (g_cc > 0 &&
                       ((L->text[g_cc-1] >= 'a' && L->text[g_cc-1] <= 'z') ||
                        (L->text[g_cc-1] >= 'A' && L->text[g_cc-1] <= 'Z') ||
                        (L->text[g_cc-1] >= '0' && L->text[g_cc-1] <= '9') ||
                        L->text[g_cc-1] == '_')) g_cc--;
            }
            break;
        }
        case 'G': g_cr = g_n_lines - 1; g_cc = 0; break;
        case 'g': g_pending = 'g'; break;
        case 'd': g_pending = 'd'; break;
        case 'y': g_pending = 'y'; break;
        case 'x':
            line_delete_char(g_cr, g_cc);
            g_modified = 1;
            break;
        case 'i':
            g_mode = MODE_INSERT;
            break;
        case 'I':
            g_cc = 0;
            g_mode = MODE_INSERT;
            break;
        case 'a':
            if (g_cc < g_lines[g_cr].len) g_cc++;
            g_mode = MODE_INSERT;
            break;
        case 'A':
            g_cc = g_lines[g_cr].len;
            g_mode = MODE_INSERT;
            break;
        case 'o':
            buffer_insert_line(g_cr + 1);
            g_cr++; g_cc = 0;
            g_modified = 1;
            g_mode = MODE_INSERT;
            break;
        case 'O':
            buffer_insert_line(g_cr);
            g_cc = 0;
            g_modified = 1;
            g_mode = MODE_INSERT;
            break;
        case 'p':
        case 'P': {
            if (g_yank_len == 0) break;
            int target = (c == 'p') ? g_cr + 1 : g_cr;
            if (g_yank_is_line) {
                buffer_insert_line(target);
                struct line *L = &g_lines[target];
                line_grow(L, g_yank_len + 1);
                for (int i = 0; i < g_yank_len; i++) L->text[i] = g_yank_buf[i];
                L->len = g_yank_len;
                g_cr = target;
                g_cc = 0;
            } else {
                /* Inline paste — insert chars at cursor. */
                for (int i = 0; i < g_yank_len; i++)
                    line_insert_char(g_cr, g_cc + i, g_yank_buf[i]);
                g_cc += g_yank_len;
            }
            g_modified = 1;
            break;
        }
        case ':':
            g_mode = MODE_COMMAND;
            g_cmd_lead = ':';
            g_cmd_len = 0;
            break;
        case '/':
            g_mode = MODE_COMMAND;
            g_cmd_lead = '/';
            g_cmd_len = 0;
            break;
        case 'n':
            if (g_search_len) {
                int sc = g_cc + 1;
                int sr = g_cr;
                if (sc > g_lines[sr].len) { sc = 0; sr++; if (sr >= g_n_lines) sr = 0; }
                if (!search_next(sr, sc, 1)) set_status("pattern not found");
            }
            break;
        case 'N':
            if (g_search_len) {
                int sc = g_cc - 1;
                int sr = g_cr;
                if (sc < 0) { sr--; if (sr < 0) sr = g_n_lines - 1; sc = g_lines[sr].len - 1; }
                if (!search_next(sr, sc, 0)) set_status("pattern not found");
            }
            break;
        case 27:    /* ESC */
            g_pending = 0;
            break;
        default:
            break;
    }
    clamp_cursor();
}

static void handle_insert(int c) {
    if (c == 27) {            /* ESC → back to normal */
        g_mode = MODE_NORMAL;
        if (g_cc > 0 && g_cc >= g_lines[g_cr].len) g_cc = g_lines[g_cr].len - 1;
        clamp_cursor();
        return;
    }
    if (c == '\b' || c == 127) {     /* backspace */
        if (g_cc > 0) {
            line_delete_char(g_cr, g_cc - 1);
            g_cc--;
            g_modified = 1;
        } else if (g_cr > 0) {
            /* Backspace at column 0: merge with previous line. */
            int prev_len = g_lines[g_cr - 1].len;
            line_join(g_cr - 1);
            g_cr--;
            g_cc = prev_len;
            g_modified = 1;
        }
        return;
    }
    if (c == '\n' || c == '\r') {
        line_split(g_cr, g_cc);
        g_cr++;
        g_cc = 0;
        g_modified = 1;
        return;
    }
    if (c >= 0x20 && c < 0x7F) {
        line_insert_char(g_cr, g_cc, (char)c);
        g_cc++;
        g_modified = 1;
    }
}

static void handle_command(int c) {
    if (c == 27) {            /* ESC → cancel */
        g_mode = MODE_NORMAL;
        g_cmd_len = 0;
        return;
    }
    if (c == '\n' || c == '\r') {
        if (g_cmd_lead == ':') run_colon_command(g_cmd_buf, g_cmd_len);
        else                   run_search(g_cmd_buf, g_cmd_len);
        g_mode = MODE_NORMAL;
        g_cmd_len = 0;
        clamp_cursor();
        return;
    }
    if (c == '\b' || c == 127) {
        if (g_cmd_len > 0) g_cmd_len--;
        else { g_mode = MODE_NORMAL; }     /* empty backspace = cancel */
        return;
    }
    if (c >= 0x20 && c < 0x7F && g_cmd_len < (int)sizeof(g_cmd_buf) - 1) {
        g_cmd_buf[g_cmd_len++] = (char)c;
    }
}

/* ---- main ----------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: vi <file>\n");
        return 1;
    }
    my_strncpy(g_filename, argv[1], (int)sizeof(g_filename));

    buffer_init();
    file_load(g_filename);
    if (g_n_lines == 0) buffer_init();
    g_cr = g_cc = g_top_row = 0;
    g_mode = MODE_NORMAL;

    int prev_mode = tty_get_mode();
    tty_set_mode(TTY_RAW);
    sys_tty_clear();
    redraw();

    for (;;) {
        char buf;
        int  n = sys_read(0, &buf, 1);
        if (n <= 0) break;
        int c = (unsigned char)buf;

        if (g_mode == MODE_NORMAL)       handle_normal (c);
        else if (g_mode == MODE_INSERT)  handle_insert (c);
        else                              handle_command(c);

        if (g_quit) break;
        redraw();
    }

    /* Tidy up: leave the bottom row clean and restore cooked TTY. */
    sys_tty_clear();
    sys_tty_cursor(0, 0);
    tty_set_mode(prev_mode);
    return 0;
}
