/*
 * wmterm.c — session 134 / Path C phase 27 terminal-emulator
 * WM client.
 *
 *   wmterm [seconds]   open a 60×24 ANSI-ish terminal window
 *                      running sh.elf.  Type into the focused
 *                      window; output appears as text.  Default
 *                      lifetime 120 s.
 *
 * Design
 * ======
 *   - sys_openpty + sys_fork: the child dup2's the slave to its
 *     stdio fds and execs sh.elf.  The master fd stays with
 *     wmterm; it's marked non-blocking via sys_fd_nb so the main
 *     loop can poll it.
 *   - On every tick: drain WM_EV_KEY events and write each byte
 *     to the master fd (so the shell sees keystrokes).  Then
 *     non-blocking read from the master and feed every byte
 *     through a small VT state machine that:
 *       printable → place at cursor, advance col, wrap, maybe scroll
 *       '\n'       → newline + advance row, maybe scroll
 *       '\r'       → col = 0
 *       '\b'       → col--
 *       ESC [...m  → strip (colour codes, ignored)
 *       ESC [...J  → strip (clear, ignored)
 *       other ESC  → strip until the final byte
 *   - Repaint the 60×24 grid each frame.
 *
 * No mouse handling inside the terminal (the WM still handles
 * focus / drag / resize on the title bar + frame).
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W        540    /* 540 = 60 * 8 + 60 px margin/decoration */
/* Session 162 — WIN_H was 240 with the comment "= 24*10 + 4 px
 * margin", but the math forgot the HDR_H+4 = 24 px taken up by the
 * internal title strip above the grid.  With WIN_H=240 only the
 * top 21 grid rows fit on screen; rows 21-23 (including the row
 * sh.elf renders the new prompt on after `ls /` scrolls everything
 * up) were clipped off the bottom of the surface.  Correct:
 * GRID_Y(24) + ROWS(24)*LINE_H(10) + bottom margin = 264..270. */
#define WIN_H        270    /* 270 = 24 (header) + 24*10 (rows) + 6 margin */
#define HDR_H        20
#define COLS         60
#define ROWS         24
#define LINE_H       10
#define CELL_W       8
#define GRID_X       6
#define GRID_Y       (HDR_H + 4)
#define READ_BUF     2048

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

/* Text grid + cursor. */
static char  g_grid[ROWS][COLS];
static int   g_cur_col;
static int   g_cur_row;
/* VT state: 0=normal, 1=saw ESC, 2=saw ESC [ */
static int   g_vt_state;

/* Session 159 — scrollback ring.  Every row that grid_scroll pushes
 * off the top of g_grid lands here so the user can PgUp/PgDn through
 * earlier output.  500 rows × 60 cols = 30 KB BSS, comfortably below
 * the user-binary BSS cap.  g_view_offset is the live-tail offset:
 * 0 means rendering shows g_grid (current); positive means scroll
 * back that many rows into history. */
#define SB_ROWS    500
#define PAGE_STEP  12               /* half a screen per PgUp/PgDn */
static char g_sb[SB_ROWS][COLS];
static int  g_sb_count;             /* rows pushed so far (0..SB_ROWS) */
static int  g_sb_head;              /* index of oldest row in g_sb */
static int  g_view_offset;          /* rows scrolled back from live tail */

/* Session 166 — ANSI colour support.  Each cell has a parallel
 * fg/bg palette index in [0..15] or 0xFF for "use defaults" (the
 * old sage-green-on-near-black look).  Stored in separate arrays
 * mirroring the char arrays so grid scrolling / scrollback are
 * simple parallel copies. */
#define COLOR_DEFAULT 0xFF
static unsigned char g_grid_fg[ROWS][COLS];
static unsigned char g_grid_bg[ROWS][COLS];
static unsigned char g_sb_fg[SB_ROWS][COLS];
static unsigned char g_sb_bg[SB_ROWS][COLS];
/* Active SGR state.  -1 means "default"; 0..15 maps into the
 * palette below.  Updated by CSI m sequences in vt_feed. */
static int g_cur_fg = -1;
static int g_cur_bg = -1;

/* Standard 16-colour VGA-ish palette.  Indices 0..7 are the
 * non-bright SGR foregrounds (30..37); 8..15 are the bright
 * versions (90..97 or "bold" + a non-bright colour).  Same byte
 * encoding the kernel console uses, so colour-aware programs
 * render the same in both contexts. */
static const unsigned int g_palette[16] = {
    0x000000, 0xCC0000, 0x00CC00, 0xCCCC00,
    0x0000CC, 0xCC00CC, 0x00CCCC, 0xC0C0C0,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

/* Session 167 — 256-colour resolution.  Index 0..15 returns the
 * 16-colour palette above.  16..231 is the 6×6×6 RGB cube xterm
 * defines: index = 16 + 36*r + 6*g + b for r,g,b in [0..5].  Each
 * channel maps to 0 (when 0) or 55 + 40*n (when n in 1..5).
 * 232..255 is a 24-step grayscale ramp at v = 8 + 10*(i - 232).
 *
 * Index 255 is unreachable through normal SGR because it collides
 * with our COLOR_DEFAULT sentinel; user gets palette index 254
 * (almost-white grayscale) or palette index 15 (bright white)
 * instead.  Acceptable single-cell loss. */
static unsigned int palette256(int i) {
    if (i < 0 || i > 255) return 0;
    if (i < 16) return g_palette[i];
    if (i < 232) {
        i -= 16;
        int r = (i / 36) % 6;
        int g = (i / 6)  % 6;
        int b =  i       % 6;
        int R = r ? r * 40 + 55 : 0;
        int G = g ? g * 40 + 55 : 0;
        int B = b ? b * 40 + 55 : 0;
        return ((unsigned)R << 16) | ((unsigned)G << 8) | (unsigned)B;
    }
    int v = 8 + (i - 232) * 10;
    return ((unsigned)v << 16) | ((unsigned)v << 8) | (unsigned)v;
}

/* Session 167 — per-cell rendering attributes packed into one byte.
 * Bit 0 = underline (SGR 4), bit 1 = strikethrough (SGR 9),
 * bit 2 = italic (SGR 3 — stored but not rendered; the 8x8 bitmap
 * font has no italic glyphs).  Stays 0 by default. */
#define ATTR_UNDERLINE 0x01
#define ATTR_STRIKE    0x02
#define ATTR_ITALIC    0x04
static unsigned char g_grid_attr[ROWS][COLS];
static unsigned char g_sb_attr[SB_ROWS][COLS];
static unsigned char g_cur_attr = 0;     /* active SGR attribute mask */

static void grid_clear(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            g_grid[r][c]      = 0;
            g_grid_fg[r][c]   = COLOR_DEFAULT;
            g_grid_bg[r][c]   = COLOR_DEFAULT;
            g_grid_attr[r][c] = 0;
        }
    g_cur_col = 0;
    g_cur_row = 0;
    g_sb_count = 0;
    g_sb_head  = 0;
    g_view_offset = 0;
    g_cur_fg = -1;
    g_cur_bg = -1;
    g_cur_attr = 0;
}

/* Shift every row up by one; the top row goes into the scrollback
 * ring, the bottom row becomes blank. */
static void grid_scroll(void) {
    int slot = (g_sb_head + g_sb_count) % SB_ROWS;
    for (int c = 0; c < COLS; c++) {
        g_sb[slot][c]      = g_grid[0][c];
        g_sb_fg[slot][c]   = g_grid_fg[0][c];
        g_sb_bg[slot][c]   = g_grid_bg[0][c];
        g_sb_attr[slot][c] = g_grid_attr[0][c];
    }
    if (g_sb_count < SB_ROWS) g_sb_count++;
    else g_sb_head = (g_sb_head + 1) % SB_ROWS;

    for (int r = 0; r < ROWS - 1; r++)
        for (int c = 0; c < COLS; c++) {
            g_grid[r][c]      = g_grid[r+1][c];
            g_grid_fg[r][c]   = g_grid_fg[r+1][c];
            g_grid_bg[r][c]   = g_grid_bg[r+1][c];
            g_grid_attr[r][c] = g_grid_attr[r+1][c];
        }
    for (int c = 0; c < COLS; c++) {
        g_grid[ROWS-1][c]      = 0;
        g_grid_fg[ROWS-1][c]   = COLOR_DEFAULT;
        g_grid_bg[ROWS-1][c]   = COLOR_DEFAULT;
        g_grid_attr[ROWS-1][c] = 0;
    }
}

/* Return a pointer to the COLS-wide row that should appear at
 * visible-row `r` (0 = top of window, ROWS-1 = bottom), respecting
 * g_view_offset.  Rows above g_sb_count + ROWS - g_view_offset live
 * in g_sb; rows from there to the bottom live in g_grid. */
static const char *visible_row(int r) {
    int abs_row = (g_sb_count - g_view_offset) + r;
    if (abs_row < 0) return 0;                    /* above oldest */
    if (abs_row < g_sb_count) {
        int slot = (g_sb_head + abs_row) % SB_ROWS;
        return g_sb[slot];
    }
    int gr = abs_row - g_sb_count;
    if (gr < ROWS) return g_grid[gr];
    return 0;
}

/* Session 166 — parallel colour accessors.  Same shape as
 * visible_row + get_abs_row but for the fg/bg palette index
 * arrays.  Returns NULL if the row isn't in the window or has
 * been pushed past the scrollback ring's tail. */
static const unsigned char *visible_fg_row(int r) {
    int abs_row = (g_sb_count - g_view_offset) + r;
    if (abs_row < 0) return 0;
    if (abs_row < g_sb_count)
        return g_sb_fg[(g_sb_head + abs_row) % SB_ROWS];
    int gr = abs_row - g_sb_count;
    return (gr < ROWS) ? g_grid_fg[gr] : 0;
}

static const unsigned char *visible_bg_row(int r) {
    int abs_row = (g_sb_count - g_view_offset) + r;
    if (abs_row < 0) return 0;
    if (abs_row < g_sb_count)
        return g_sb_bg[(g_sb_head + abs_row) % SB_ROWS];
    int gr = abs_row - g_sb_count;
    return (gr < ROWS) ? g_grid_bg[gr] : 0;
}

static const unsigned char *visible_attr_row(int r) {
    int abs_row = (g_sb_count - g_view_offset) + r;
    if (abs_row < 0) return 0;
    if (abs_row < g_sb_count)
        return g_sb_attr[(g_sb_head + abs_row) % SB_ROWS];
    int gr = abs_row - g_sb_count;
    return (gr < ROWS) ? g_grid_attr[gr] : 0;
}

/* Session 165 — absolute-row variant of visible_row.  Selection is
 * stored in absolute row indices (across the whole sb+grid stream)
 * so the range stays correct when the user scrolls.  Returns 0 if
 * the row has been pushed past the ring's capacity. */
static const char *get_abs_row(int abs_idx) {
    if (abs_idx < 0) return 0;
    if (abs_idx < g_sb_count) {
        int slot = (g_sb_head + abs_idx) % SB_ROWS;
        return g_sb[slot];
    }
    int gr = abs_idx - g_sb_count;
    if (gr >= 0 && gr < ROWS) return g_grid[gr];
    return 0;
}

/* Session 165 — selection state.  Drag with the left mouse button
 * marks a range; releasing copies the selected text to the system
 * clipboard.  Stored in absolute row coords so the highlight stays
 * pinned to the right text when the user scrolls.  -1 anywhere
 * means "no selection". */
static int g_sel_anchor_row = -1, g_sel_anchor_col = -1;
static int g_sel_head_row   = -1, g_sel_head_col   = -1;
static int g_sel_dragging = 0;        /* button held since press */
/* Session 167 — double-click word-select tracking.  Tick number +
 * cell coords of the most recent PRESS; a second PRESS within 10
 * ticks and ±1 column triggers word selection. */
static int g_last_press_tick = -100;
static int g_last_press_row  = -1;
static int g_last_press_col  = -1;

static int sel_active(void) {
    return g_sel_anchor_row >= 0 && g_sel_head_row >= 0
        && (g_sel_anchor_row != g_sel_head_row
            || g_sel_anchor_col != g_sel_head_col);
}

/* Normalise selection into (lo_row, lo_col) <= (hi_row, hi_col).
 * lo_col only matters when lo_row == hi_row (single-row selection);
 * for multi-row, the first row goes from lo_col to end-of-line, the
 * last row from start-of-line to hi_col, middle rows are full. */
static void sel_bounds(int *lr, int *lc, int *hr, int *hc) {
    int ar = g_sel_anchor_row, ac = g_sel_anchor_col;
    int br = g_sel_head_row,   bc = g_sel_head_col;
    if (ar > br || (ar == br && ac > bc)) {
        *lr = br; *lc = bc; *hr = ar; *hc = ac;
    } else {
        *lr = ar; *lc = ac; *hr = br; *hc = bc;
    }
}

static int in_selection(int abs_r, int col) {
    if (!sel_active()) return 0;
    int lr, lc, hr, hc;
    sel_bounds(&lr, &lc, &hr, &hc);
    if (abs_r < lr || abs_r > hr) return 0;
    if (lr == hr)              return col >= lc && col <= hc;
    if (abs_r == lr)           return col >= lc;
    if (abs_r == hr)           return col <= hc;
    return 1;
}

/* Drop trailing blanks from a row buffer, return new length. */
static int row_trim_len(const char *row) {
    int n = COLS;
    while (n > 0 && (row[n - 1] == 0 || row[n - 1] == ' ')) n--;
    return n;
}

/* Build the currently-selected text into out_buf and return the
 * byte length.  Caller's buffer must be large enough; we cap at
 * out_cap and return -1 if the selection would overflow. */
static int sel_copy_text(char *out_buf, int out_cap) {
    if (!sel_active()) return 0;
    int lr, lc, hr, hc;
    sel_bounds(&lr, &lc, &hr, &hc);
    int n = 0;
    for (int r = lr; r <= hr; r++) {
        const char *row = get_abs_row(r);
        if (!row) continue;
        int start = (r == lr) ? lc : 0;
        int end   = (r == hr) ? hc + 1 : COLS;
        /* Trim trailing blanks on multi-row selections only — keep
         * the user's spaces on a single-row pick. */
        if (lr != hr) {
            int tn = row_trim_len(row);
            if (end > tn) end = tn;
        }
        for (int c = start; c < end; c++) {
            if (n >= out_cap - 1) return -1;
            char ch = row[c];
            out_buf[n++] = ch ? ch : ' ';
        }
        if (r < hr) {
            if (n >= out_cap - 1) return -1;
            out_buf[n++] = '\n';
        }
    }
    out_buf[n] = 0;
    return n;
}

/* Session 168 — Shift+arrow / Shift+Home / Shift+End selection
 * extender.  If no selection is active, seed the anchor at the
 * shell's current cursor (rendered as the blinking caret); then
 * move the head per direction.  Re-copies the resulting selection
 * to the clipboard each step so the user can paste mid-stride. */
static void sel_kbd_extend(char direction) {
    int caret_row = g_sb_count + g_cur_row;
    if (!sel_active()) {
        g_sel_anchor_row = caret_row;
        g_sel_anchor_col = g_cur_col;
        g_sel_head_row   = caret_row;
        g_sel_head_col   = g_cur_col;
        g_sel_dragging   = 0;
    }
    switch (direction) {
        case 'A':                                       /* Up */
            if (g_sel_head_row > 0) g_sel_head_row--;
            break;
        case 'B':                                       /* Down */
            if (g_sel_head_row < g_sb_count + ROWS - 1)
                g_sel_head_row++;
            break;
        case 'C':                                       /* Right */
            if (g_sel_head_col < COLS - 1)
                g_sel_head_col++;
            else if (g_sel_head_row < g_sb_count + ROWS - 1) {
                g_sel_head_row++;
                g_sel_head_col = 0;
            }
            break;
        case 'D':                                       /* Left */
            if (g_sel_head_col > 0)
                g_sel_head_col--;
            else if (g_sel_head_row > 0) {
                g_sel_head_row--;
                g_sel_head_col = COLS - 1;
            }
            break;
        case 'H':                                       /* Home — line start */
            g_sel_head_col = 0;
            break;
        case 'F':                                       /* End — line end */
            g_sel_head_col = COLS - 1;
            break;
    }
    /* Auto-copy the in-progress selection so the user can paste
     * without an extra gesture. */
    if (sel_active()) {
        char cbuf[2048];
        int n = sel_copy_text(cbuf, sizeof(cbuf));
        if (n > 0) sys_clipboard_set(cbuf, n);
    }
}

/* Convert surface-local pixel (sx, sy) to grid (abs_row, col).
 * Returns 0 if outside the grid (above header / off the bottom). */
static int xy_to_cell(int sx, int sy, int *out_abs_row, int *out_col) {
    if (sx < GRID_X || sy < GRID_Y) return 0;
    int c = (sx - GRID_X) / CELL_W;
    int r = (sy - GRID_Y) / LINE_H;
    if (c < 0) c = 0;
    if (c >= COLS) c = COLS - 1;
    if (r < 0) r = 0;
    if (r >= ROWS) r = ROWS - 1;
    *out_col     = c;
    *out_abs_row = (g_sb_count - g_view_offset) + r;
    return 1;
}

static void scroll_up(void) {
    g_view_offset += PAGE_STEP;
    if (g_view_offset > g_sb_count) g_view_offset = g_sb_count;
}

static void scroll_down(void) {
    g_view_offset -= PAGE_STEP;
    if (g_view_offset < 0) g_view_offset = 0;
}

/* Place a printable character at cursor; advance.  Wrap/scroll
 * as needed. */
static void grid_putc_printable(char c) {
    if (g_cur_col >= COLS) {
        g_cur_col = 0;
        g_cur_row++;
        if (g_cur_row >= ROWS) {
            grid_scroll();
            g_cur_row = ROWS - 1;
        }
    }
    g_grid[g_cur_row][g_cur_col]      = c;
    g_grid_fg[g_cur_row][g_cur_col]   = (g_cur_fg < 0) ? COLOR_DEFAULT
                                                     : (unsigned char)g_cur_fg;
    g_grid_bg[g_cur_row][g_cur_col]   = (g_cur_bg < 0) ? COLOR_DEFAULT
                                                     : (unsigned char)g_cur_bg;
    g_grid_attr[g_cur_row][g_cur_col] = g_cur_attr;
    g_cur_col++;
}

static void grid_newline(void) {
    g_cur_col = 0;
    g_cur_row++;
    if (g_cur_row >= ROWS) {
        grid_scroll();
        g_cur_row = ROWS - 1;
    }
}

/* Feed one byte through the VT-100ish state machine.
 *
 * Session 161 — handles a small set of CSI sequences instead of
 * silently stripping them all, so e.g. sh.elf's `redraw_line` can
 * emit `ESC [ K` to clear from cursor to end-of-line when the user
 * backspaces.  Without that handling, redraw_line wrote the
 * shortened buffer over the longer one but the trailing characters
 * stayed visible, and the user saw backspace as "doing nothing".
 *
 * Still strips: everything else (colour codes, scroll-region setup,
 * full clear-screen — wmterm doesn't render colours and doesn't
 * track a viewport offset distinct from g_view_offset). */
/* Session 166 — buffer grew to 16 so SGR sequences like
 * "CSI 1;31m" or "CSI 38;5;200m" fit.  Old buffer was 8. */
static char    g_csi_param[16];
static int     g_csi_param_len;

static int csi_get_param(int dflt) {
    /* Parse the buffered numeric parameter (possibly empty).  We
     * only care about the first parameter; semicolon-separated
     * multi-param sequences fall back to dflt. */
    int v = 0, any = 0;
    for (int i = 0; i < g_csi_param_len; i++) {
        char c = g_csi_param[i];
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
        any = 1;
    }
    return any ? v : dflt;
}

/* Session 166 — apply each semicolon-separated SGR parameter to the
 * current fg/bg state.  ECMA-48 SGR (Select Graphic Rendition):
 *   0       reset
 *   1       bold (we map to "bright" by OR'ing 0x08 into the fg index)
 *   22      cancel bold
 *   30..37  set foreground colour 0..7
 *   38;5;N  256-colour foreground (we accept the prefix and use
 *           N mod 16 — close enough for most --color=auto output)
 *   39      reset foreground to default
 *   40..47  set background colour 0..7
 *   49      reset background to default
 *   90..97  bright foreground (== 8..15)
 *   100..107 bright background
 * Unknown params are ignored. */
/* Helper: parse one decimal token from g_csi_param starting at *idx,
 * advancing past digits.  Returns the value (0 if no digits). */
static int sgr_parse_num(int *idx) {
    int v = 0;
    while (*idx < g_csi_param_len
           && g_csi_param[*idx] >= '0'
           && g_csi_param[*idx] <= '9') {
        v = v * 10 + (g_csi_param[*idx] - '0');
        (*idx)++;
    }
    return v;
}

static void apply_sgr(void) {
    int idx = 0;
    /* Empty sequence (CSI m with no params) is treated as CSI 0 m. */
    if (g_csi_param_len == 0) {
        g_cur_fg = -1; g_cur_bg = -1; g_cur_attr = 0;
        return;
    }
    while (idx < g_csi_param_len) {
        int start = idx;
        int v = sgr_parse_num(&idx);
        if (idx == start) v = 0;          /* "CSI ;31m" — empty = 0 */
        if (v == 0)                       { g_cur_fg = -1; g_cur_bg = -1;
                                            g_cur_attr = 0; }
        else if (v == 1) {                 /* bold = bright fg */
            int f = (g_cur_fg < 0) ? 7 : (g_cur_fg & 0x07);
            g_cur_fg = f | 0x08;
        }
        else if (v == 3)                  g_cur_attr |= ATTR_ITALIC;
        else if (v == 4)                  g_cur_attr |= ATTR_UNDERLINE;
        else if (v == 9)                  g_cur_attr |= ATTR_STRIKE;
        else if (v == 22) {                /* cancel bold */
            if (g_cur_fg >= 0 && g_cur_fg < 16) g_cur_fg &= 0x07;
        }
        else if (v == 23)                 g_cur_attr &= (unsigned char)~ATTR_ITALIC;
        else if (v == 24)                 g_cur_attr &= (unsigned char)~ATTR_UNDERLINE;
        else if (v == 29)                 g_cur_attr &= (unsigned char)~ATTR_STRIKE;
        else if (v >= 30 && v <= 37)       g_cur_fg = v - 30;
        else if (v == 38) {
            /* 38;5;N — 256-colour foreground.  38;2;R;G;B — direct
             * RGB; not stored as full RGB so we approximate by
             * picking the nearest of the 6×6×6 cube. */
            if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
            int mode = sgr_parse_num(&idx);
            if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
            if (mode == 5) {
                int n = sgr_parse_num(&idx);
                /* Clamp to 0..254; 255 collides with COLOR_DEFAULT. */
                if (n < 0) n = 0;
                if (n > 254) n = 254;
                g_cur_fg = n;
            } else if (mode == 2) {
                int R = sgr_parse_num(&idx);
                if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
                int G = sgr_parse_num(&idx);
                if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
                int B = sgr_parse_num(&idx);
                /* Map to nearest 6×6×6 cube point. */
                int r6 = (R * 5 + 127) / 255;
                int g6 = (G * 5 + 127) / 255;
                int b6 = (B * 5 + 127) / 255;
                g_cur_fg = 16 + 36*r6 + 6*g6 + b6;
            }
        }
        else if (v == 39)                  g_cur_fg = -1;
        else if (v >= 40 && v <= 47)       g_cur_bg = v - 40;
        else if (v == 48) {
            /* 48;5;N or 48;2;R;G;B — background variants of 38. */
            if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
            int mode = sgr_parse_num(&idx);
            if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
            if (mode == 5) {
                int n = sgr_parse_num(&idx);
                if (n < 0) n = 0;
                if (n > 254) n = 254;
                g_cur_bg = n;
            } else if (mode == 2) {
                int R = sgr_parse_num(&idx);
                if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
                int G = sgr_parse_num(&idx);
                if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
                int B = sgr_parse_num(&idx);
                int r6 = (R * 5 + 127) / 255;
                int g6 = (G * 5 + 127) / 255;
                int b6 = (B * 5 + 127) / 255;
                g_cur_bg = 16 + 36*r6 + 6*g6 + b6;
            }
        }
        else if (v == 49)                  g_cur_bg = -1;
        else if (v >= 90 && v <= 97)       g_cur_fg = (v - 90) | 0x08;
        else if (v >= 100 && v <= 107)     g_cur_bg = (v - 100) | 0x08;
        /* unknown — drop silently */
        if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
        else break;
    }
}

static void csi_dispatch(char final) {
    switch (final) {
        case 'K': {
            /* Erase in Line — parameter selects mode:
             *   0 (default) : clear from cursor to end of line
             *   1           : clear from beginning of line to cursor
             *   2           : clear entire line
             * sh.elf's redraw_line only ever emits the no-param form
             * (mode 0), but the other modes are cheap. */
            int mode = csi_get_param(0);
            if (mode == 0) {
                for (int c = g_cur_col; c < COLS; c++)
                    g_grid[g_cur_row][c] = 0;
            } else if (mode == 1) {
                for (int c = 0; c <= g_cur_col && c < COLS; c++)
                    g_grid[g_cur_row][c] = 0;
            } else if (mode == 2) {
                for (int c = 0; c < COLS; c++)
                    g_grid[g_cur_row][c] = 0;
            }
            return;
        }
        case 'J': {
            /* Session 163 — Erase in Display.  Modes:
             *   0 (default) : clear from cursor to end of screen
             *   1           : clear from start of screen to cursor
             *   2           : clear entire visible screen
             *   3           : clear screen AND the scrollback ring
             * sh.elf's `clear` builtin emits CSI 2J (paired with
             * CSI H to home the cursor — see case 'H' below). */
            int mode = csi_get_param(0);
            if (mode == 2 || mode == 3) {
                for (int r = 0; r < ROWS; r++)
                    for (int c = 0; c < COLS; c++)
                        g_grid[r][c] = 0;
                if (mode == 3) {
                    g_sb_count = 0;
                    g_sb_head  = 0;
                    g_view_offset = 0;
                }
            } else if (mode == 0) {
                /* cursor → screen end */
                for (int c = g_cur_col; c < COLS; c++)
                    g_grid[g_cur_row][c] = 0;
                for (int r = g_cur_row + 1; r < ROWS; r++)
                    for (int c = 0; c < COLS; c++)
                        g_grid[r][c] = 0;
            } else if (mode == 1) {
                /* screen start → cursor */
                for (int r = 0; r < g_cur_row; r++)
                    for (int c = 0; c < COLS; c++)
                        g_grid[r][c] = 0;
                for (int c = 0; c <= g_cur_col && c < COLS; c++)
                    g_grid[g_cur_row][c] = 0;
            }
            return;
        }
        case 'H':
        case 'f': {
            /* Session 163 — Cursor Position.  ANSI uses 1-based
             * row/col; we store 0-based g_cur_row / g_cur_col.
             * Parameters are `row;col`; csi_get_param only returns
             * the first (row), which is enough for the no-arg
             * (default row=1 col=1) home form sh's clear emits. */
            int row = csi_get_param(1);
            if (row < 1)       row = 1;
            if (row > ROWS)    row = ROWS;
            g_cur_row = row - 1;
            g_cur_col = 0;          /* default col when only row given */
            return;
        }
        case 'G': {
            /* Session 168 — Cursor Horizontal Absolute.  Move the
             * cursor to column N (1-based) on the current row.
             * sh.elf's position_cursor emits this for mid-line
             * edits (backspace, Ctrl-A, history nav).  Before
             * this, the only thing sh emitted was sys_tty_cursor
             * (kernel-console-only); the caret in wmterm stayed
             * at end-of-line through every mid-line edit. */
            int col = csi_get_param(1);
            if (col < 1)    col = 1;
            if (col > COLS) col = COLS;
            g_cur_col = col - 1;
            return;
        }
        case 'm':
            /* Session 166 — Select Graphic Rendition.  Updates the
             * current fg/bg state used by subsequent printable chars. */
            apply_sgr();
            return;
        /* Other CSI: silently strip (cursor save/restore, scroll
         * region setup, etc).  No grid mutation. */
        default: return;
    }
}

static void vt_feed(unsigned char b) {
    if (g_vt_state == 1) {
        if (b == '[') { g_vt_state = 2; g_csi_param_len = 0; return; }
        g_vt_state = 0;
        return;
    }
    if (g_vt_state == 2) {
        /* Parameter / intermediate bytes (0x20..0x3F): buffer up
         * to 7 so csi_get_param has something to parse. */
        if (b >= 0x20 && b <= 0x3F) {
            if (g_csi_param_len < (int)sizeof(g_csi_param))
                g_csi_param[g_csi_param_len++] = (char)b;
            return;
        }
        /* Final byte (0x40..0x7E) — dispatch + reset. */
        if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch((char)b);
            g_vt_state = 0;
            g_csi_param_len = 0;
        }
        return;
    }
    switch (b) {
        case 27:  g_vt_state = 1; return;      /* ESC */
        case '\n': grid_newline(); return;
        case '\r': g_cur_col = 0; return;
        case '\b': if (g_cur_col > 0) g_cur_col--;
                   g_grid[g_cur_row][g_cur_col] = ' ';
                   return;
        case '\t':
            do { grid_putc_printable(' '); } while (g_cur_col % 8 != 0);
            return;
        default:
            if (b >= 0x20 && b <= 0x7E) grid_putc_printable((char)b);
            return;
    }
}

/* Session 157 — `wmterm -v <secs>` (or env WMTERM_VERBOSE=1) prints
 * one line per FOCUS/UNFOCUS/KEY/read event.  Used by the smoke test
 * to verify the PTY-non-block fix round-trips events end-to-end;
 * leaving it always-on would spam the launching console with one
 * line per keystroke, so the default is quiet. */
static int g_verbose = 0;

/* Session 159 — input-side CSI parser.  Single PgUp/PgDn presses
 * arrive from wmd as four separate WM_EV_KEY events (ESC, '[', '5'
 * or '6', '~').  We buffer until the sequence is complete; if it
 * matches a wmterm-special, we consume it locally; otherwise we
 * flush the buffered bytes to the PTY in order, so e.g. arrow keys
 * still reach sh.elf for history navigation. */
static unsigned char g_kbd_esc[8];
static int           g_kbd_esc_len;

static void key_byte(int master, unsigned char b) {
    if (g_kbd_esc_len == 0) {
        if (b == 27) { g_kbd_esc[0] = 27; g_kbd_esc_len = 1; return; }
        sys_write(master, &b, 1);
        return;
    }
    /* Mid-ESC.  Buffer first, then dispatch on the new state. */
    if (g_kbd_esc_len < (int)sizeof(g_kbd_esc))
        g_kbd_esc[g_kbd_esc_len++] = b;

    /* Second byte: must be '[' for CSI.  '[' means "keep going";
     * anything else means this was a bare ESC + literal so flush both
     * to the PTY and go back to idle. */
    if (g_kbd_esc_len == 2) {
        if (b == '[') return;                  /* CSI start, keep collecting */
        sys_write(master, g_kbd_esc, g_kbd_esc_len);
        g_kbd_esc_len = 0;
        return;
    }
    /* Parameter / intermediate bytes (0x20..0x3F): keep collecting,
     * with an overrun safety valve. */
    if (b >= 0x20 && b <= 0x3F) {
        if (g_kbd_esc_len >= (int)sizeof(g_kbd_esc)) {
            sys_write(master, g_kbd_esc, g_kbd_esc_len);
            g_kbd_esc_len = 0;
        }
        return;
    }
    /* Final byte (0x40..0x7E): sequence complete.  Match wmterm
     * specials (PgUp/PgDn, Shift+arrow, Shift+Home/End); flush
     * everything else through to the PTY so arrows and friends
     * still work. */
    if (b >= 0x40 && b <= 0x7E) {
        if (g_kbd_esc_len == 4 && g_kbd_esc[2] == '5' && b == '~') {
            scroll_up();
        } else if (g_kbd_esc_len == 4 && g_kbd_esc[2] == '6' && b == '~') {
            scroll_down();
        } else if (g_kbd_esc_len == 6
                   && g_kbd_esc[2] == '1'
                   && g_kbd_esc[3] == ';'
                   && g_kbd_esc[4] == '2'
                   && (b == 'A' || b == 'B' || b == 'C' || b == 'D'
                       || b == 'H' || b == 'F')) {
            /* Session 168 — keyboard-driven selection.  Shift+arrow
             * / Shift+Home / Shift+End extend the selection
             * relative to the shell's current cursor position
             * (which we render with the blinking caret). */
            sel_kbd_extend((char)b);
        } else {
            sys_write(master, g_kbd_esc, g_kbd_esc_len);
        }
        g_kbd_esc_len = 0;
        return;
    }
    /* Anything else mid-sequence (e.g. another ESC, control char):
     * flush what we have and return to idle without losing bytes. */
    sys_write(master, g_kbd_esc, g_kbd_esc_len);
    g_kbd_esc_len = 0;
}

int main(int argc, char **argv) {
    int seconds = 120;
    int posarg = 1;
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'v' && !argv[1][2]) {
        g_verbose = 1;
        posarg = 2;
    }
    if (argc > posarg) seconds = my_atoi_str(argv[posarg]);
    if (seconds <= 0) seconds = 120;

    struct wm_window win;
    if (wm_open(&win, "wmterm", WIN_W, WIN_H) < 0) {
        printf("wmterm: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmterm: id=%u%s\n", win.id, g_verbose ? " (verbose)" : "");

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    /* Open a PTY and fork the shell. */
    int pty[2];
    if (sys_openpty(pty) < 0) {
        printf("wmterm: sys_openpty failed\n");
        wm_close(&win);
        return 1;
    }
    int master = pty[0];
    int slave  = pty[1];

    int pid = sys_fork();
    if (pid < 0) {
        printf("wmterm: fork failed\n");
        sys_close(master); sys_close(slave);
        wm_close(&win);
        return 1;
    }
    if (pid == 0) {
        /* Child: rewire stdio onto the slave and exec sh.elf. */
        sys_close(master);
        sys_dup2(slave, 0);
        sys_dup2(slave, 1);
        sys_dup2(slave, 2);
        if (slave > 2) sys_close(slave);
        const char *argv2[2] = { "/sh.elf", 0 };
        sys_exec("/sh.elf", argv2);
        sys_exit(127);
    }

    sys_close(slave);
    sys_fd_nb(master, 1);

    grid_clear();

    int has_focus = 0;
    int closed    = 0;
    int total_ticks = seconds * 30;
    int caret_phase = 0;

    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:
                    has_focus = 1;
                    if (g_verbose) printf("wmterm: FOCUS\n");
                    break;
                case WM_EV_UNFOCUS:
                    has_focus = 0;
                    if (g_verbose) printf("wmterm: UNFOCUS\n");
                    break;
                case WM_EV_CLOSE:   closed = 1; break;
                case WM_EV_KEY: {
                    unsigned char c = (unsigned char)ev.keycode;
                    /* Session 165 — Ctrl-V (0x16) intercepts: paste
                     * the clipboard into the PTY master at the
                     * shell prompt.  The shell never sees Ctrl-V,
                     * so its literal-next-char readline binding
                     * is dropped inside wmterm — fine trade for
                     * standard clipboard paste semantics. */
                    if (c == 0x16) {
                        char pbuf[1024];
                        int pn = sys_clipboard_get(pbuf, sizeof(pbuf));
                        if (pn > 0) sys_write(master, pbuf, pn);
                        if (g_verbose)
                            printf("wmterm: PASTE n=%d\n", pn);
                        break;
                    }
                    key_byte(master, c);
                    if (g_verbose)
                        printf("wmterm: KEY 0x%x view=%d\n",
                               (unsigned)c, g_view_offset);
                    break;
                }
                /* Session 165 — left-mouse drag selects text.
                 * MOUSE_PRESS anchors; MOUSE_MOVE while the button
                 * is held updates the head; MOUSE_RELEASE copies
                 * the selected text to the system clipboard. */
                case WM_EV_MOUSE_PRESS: {
                    if (g_verbose)
                        printf("wmterm: PRESS x=%u y=%u btn=%u\n",
                               ev.x, ev.y, ev.button);
                    /* Session 168 — middle-click paste, X11-style.
                     * Reads the current clipboard and writes it into
                     * the PTY master.  Same mechanism as Ctrl-V but
                     * triggered by the mouse so the user can paste
                     * without leaving the pointer. */
                    if (ev.button & WM_BUTTON_MIDDLE) {
                        char pbuf[1024];
                        int pn = sys_clipboard_get(pbuf, sizeof(pbuf));
                        if (pn > 0) sys_write(master, pbuf, pn);
                        if (g_verbose)
                            printf("wmterm: MPASTE n=%d\n", pn);
                        break;
                    }
                    if (ev.button & WM_BUTTON_LEFT) {
                        int r, c;
                        if (xy_to_cell((int)ev.x, (int)ev.y, &r, &c)) {
                            /* Session 167 — double-click selects the
                             * word at the click cell.  "Word" = run of
                             * printable non-whitespace chars; we scan
                             * outward from the click column until we
                             * hit a space or column boundary.  A
                             * 10-tick (~330 ms at 30 fps) threshold
                             * matches typical double-click windows. */
                            int dc_dt  = tick - g_last_press_tick;
                            int dc_dx  = c - g_last_press_col;
                            if (dc_dx < 0) dc_dx = -dc_dx;
                            int dclick = (dc_dt < 10
                                          && g_last_press_row == r
                                          && dc_dx <= 1);
                            if (dclick) {
                                const char *rowdata = get_abs_row(r);
                                if (rowdata) {
                                    int sc = c, ec = c;
                                    while (sc > 0
                                           && rowdata[sc-1] > ' '
                                           && rowdata[sc-1] < 0x7F) sc--;
                                    while (ec < COLS - 1
                                           && rowdata[ec+1] > ' '
                                           && rowdata[ec+1] < 0x7F) ec++;
                                    g_sel_anchor_row = r;
                                    g_sel_anchor_col = sc;
                                    g_sel_head_row   = r;
                                    g_sel_head_col   = ec;
                                    g_sel_dragging   = 0;
                                    /* Auto-copy on double-click, same
                                     * as drag-then-release. */
                                    if (sel_active()) {
                                        char cbuf[1024];
                                        int n = sel_copy_text(cbuf, sizeof(cbuf));
                                        if (n > 0)
                                            sys_clipboard_set(cbuf, n);
                                        if (g_verbose)
                                            printf("wmterm: WORDCOPY n=%d\n", n);
                                    }
                                }
                                g_last_press_tick = -100;
                            } else {
                                g_sel_anchor_row = r;
                                g_sel_anchor_col = c;
                                g_sel_head_row   = r;
                                g_sel_head_col   = c;
                                g_sel_dragging   = 1;
                                g_last_press_tick = tick;
                                g_last_press_row  = r;
                                g_last_press_col  = c;
                            }
                        }
                    }
                    break;
                }
                case WM_EV_MOUSE_MOVE: {
                    if (g_sel_dragging && (ev.button & WM_BUTTON_LEFT)) {
                        int r, c;
                        if (xy_to_cell((int)ev.x, (int)ev.y, &r, &c)) {
                            g_sel_head_row = r;
                            g_sel_head_col = c;
                        }
                    }
                    break;
                }
                case WM_EV_MOUSE_RELEASE: {
                    if (g_sel_dragging) {
                        g_sel_dragging = 0;
                        if (sel_active()) {
                            /* Auto-copy on release — X11-style.  Bound
                             * to one ~4 KB read since the clipboard
                             * syscall caps at that size. */
                            char cbuf[4096];
                            int n = sel_copy_text(cbuf, sizeof(cbuf));
                            if (n > 0) sys_clipboard_set(cbuf, n);
                            if (g_verbose)
                                printf("wmterm: COPY n=%d\n", n);
                        }
                    }
                    break;
                }
                case WM_EV_MOUSE_WHEEL: {
                    int delta = (int)(int32_t)ev.keycode;
                    if (delta > 0) {
                        int steps = delta / 4;
                        if (steps < 1) steps = 1;
                        for (int i = 0; i < steps; i++) scroll_up();
                    } else if (delta < 0) {
                        int steps = (-delta) / 4;
                        if (steps < 1) steps = 1;
                        for (int i = 0; i < steps; i++) scroll_down();
                    }
                    if (g_verbose)
                        printf("wmterm: WHEEL %d view=%d\n",
                               delta, g_view_offset);
                    break;
                }
                default: break;
            }
        }

        /* Drain anything the shell wrote.  Buffer is bounded so we
         * don't process an unbounded stream per frame. */
        char buf[READ_BUF];
        int n = sys_read(master, buf, sizeof(buf));
        if (n > 0) {
            if (g_verbose) {
                /* Session 161 — dump up to the first 80 chars of the
                 * read so the polish smoke can grep for the inner
                 * shell's "command not found: ab" message and verify
                 * the backspace clear-EOL CSI made it through.  Non-
                 * printable bytes show as a dot to keep the line
                 * single-line and the kernel printf simple. */
                char preview[81];
                int  plen = n < 80 ? n : 80;
                for (int i = 0; i < plen; i++) {
                    unsigned char b = (unsigned char)buf[i];
                    preview[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                }
                preview[plen] = 0;
                printf("wmterm: rd n=%d first=0x%x [%s]\n",
                       n, (unsigned)(unsigned char)buf[0], preview);
            }
            /* Session 159 — snap back to live tail whenever new shell
             * output arrives.  Without this, the user could be looking
             * at history and the cursor would move "below" the viewport,
             * which is confusing.  Matches every modern terminal. */
            if (g_view_offset != 0) g_view_offset = 0;
            for (int i = 0; i < n; i++) vt_feed((unsigned char)buf[i]);
        }

        /* Paint. */
        wm_clear(&win, 0x080808u);
        wm_fill_rect(&win, 0, 0, WIN_W, HDR_H,
                     has_focus ? 0x4080E0u : 0x404040u);
        /* Session 159 — when scrolled back, swap the header label so
         * the user knows the cursor they see isn't live.  The title
         * still includes the focus hint when unfocused. */
        const char *label;
        if (g_view_offset > 0) {
            static char sb_label[48];
            int i = 0;
            const char *prefix = "wmterm - history -";
            for (int k = 0; prefix[k]; k++) sb_label[i++] = prefix[k];
            sb_label[i++] = ' ';
            /* Render the offset as decimal digits. */
            char dbuf[8]; int dn = 0; int v = g_view_offset;
            if (v == 0) dbuf[dn++] = '0';
            while (v) { dbuf[dn++] = (char)('0' + v % 10); v /= 10; }
            while (dn--) sb_label[i++] = dbuf[dn];
            const char *suffix = " rows (PgDn to live)";
            for (int k = 0; suffix[k]; k++) sb_label[i++] = suffix[k];
            sb_label[i] = 0;
            label = sb_label;
        } else {
            label = has_focus ? "wmterm - sh.elf"
                              : "wmterm - sh.elf (click to focus)";
        }
        gfx_text(&sctx, 6, 6, label, GFX_WHITE, GFX_TRANSPARENT);

        for (int r = 0; r < ROWS; r++) {
            const char *row = visible_row(r);
            const unsigned char *fg_row   = visible_fg_row(r);
            const unsigned char *bg_row   = visible_bg_row(r);
            const unsigned char *attr_row = visible_attr_row(r);
            int abs_r = (g_sb_count - g_view_offset) + r;
            for (int c = 0; c < COLS; c++) {
                char ch = row ? row[c] : 0;
                unsigned char fgi  = fg_row   ? fg_row[c]   : COLOR_DEFAULT;
                unsigned char bgi  = bg_row   ? bg_row[c]   : COLOR_DEFAULT;
                unsigned char attr = attr_row ? attr_row[c] : 0;
                int x = GRID_X + c * CELL_W;
                int y = GRID_Y + r * LINE_H;
                int selected = in_selection(abs_r, c);
                /* Session 167 — palette256() lookup so 256-colour
                 * SGR codes (38;5;N) render with the right RGB
                 * instead of the old N-mod-16 clamp. */
                unsigned int fg = (fgi == COLOR_DEFAULT)
                                  ? 0xC0E0C0u
                                  : palette256(fgi);
                unsigned int bg = (bgi == COLOR_DEFAULT)
                                  ? (unsigned int)GFX_TRANSPARENT
                                  : palette256(bgi);
                if (selected) bg = 0x405068u;
                if (bg != (unsigned int)GFX_TRANSPARENT) {
                    wm_fill_rect(&win, x, y, CELL_W, LINE_H, bg);
                }
                if (ch) {
                    gfx_glyph(&sctx, x, y, ch, fg,
                              bg == (unsigned int)GFX_TRANSPARENT
                                  ? GFX_TRANSPARENT : bg);
                }
                /* Session 167 — underline + strikethrough overlay.
                 * 1-px lines in the cell's foreground colour.  The
                 * underline sits at the bottom of the glyph box;
                 * strikethrough crosses the middle.  Italic is
                 * stored but not rendered (the 8x8 font has no
                 * italic glyphs). */
                if (attr & ATTR_UNDERLINE)
                    wm_fill_rect(&win, x, y + LINE_H - 2, CELL_W, 1, fg);
                if (attr & ATTR_STRIKE)
                    wm_fill_rect(&win, x, y + LINE_H / 2, CELL_W, 1, fg);
            }
        }

        /* Blinking caret — only when viewing the live tail.  In
         * scrollback mode the cursor isn't where the user is looking,
         * so showing it would be misleading. */
        if (has_focus && g_view_offset == 0
            && ((caret_phase / 12) & 1) == 0) {
            int cx = GRID_X + g_cur_col * CELL_W;
            int cy = GRID_Y + g_cur_row * LINE_H;
            wm_fill_rect(&win, cx, cy, CELL_W, LINE_H - 1, 0xFFFFFFu);
        }
        caret_phase++;

        wm_present(&win);
        sys_sleep_ms(33);
    }

    sys_close(master);
    wm_close(&win);
    printf("wmterm: done\n");
    return 0;
}
