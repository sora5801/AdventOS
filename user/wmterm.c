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

static void grid_clear(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            g_grid[r][c] = 0;
    g_cur_col = 0;
    g_cur_row = 0;
    g_sb_count = 0;
    g_sb_head  = 0;
    g_view_offset = 0;
}

/* Shift every row up by one; the top row goes into the scrollback
 * ring, the bottom row becomes blank. */
static void grid_scroll(void) {
    int slot = (g_sb_head + g_sb_count) % SB_ROWS;
    for (int c = 0; c < COLS; c++) g_sb[slot][c] = g_grid[0][c];
    if (g_sb_count < SB_ROWS) g_sb_count++;
    else g_sb_head = (g_sb_head + 1) % SB_ROWS;

    for (int r = 0; r < ROWS - 1; r++)
        for (int c = 0; c < COLS; c++)
            g_grid[r][c] = g_grid[r+1][c];
    for (int c = 0; c < COLS; c++) g_grid[ROWS-1][c] = 0;
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
    g_grid[g_cur_row][g_cur_col] = c;
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
static char    g_csi_param[8];
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
        /* Other CSI: silently strip (colour `m`, cursor save/restore,
         * scroll region setup, ...).  No grid mutation. */
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
     * specials (PgUp/PgDn = '5~' / '6~'); flush everything else
     * through to the PTY so arrows and friends still work. */
    if (b >= 0x40 && b <= 0x7E) {
        if (g_kbd_esc_len == 4 && g_kbd_esc[2] == '5' && b == '~') {
            scroll_up();
        } else if (g_kbd_esc_len == 4 && g_kbd_esc[2] == '6' && b == '~') {
            scroll_down();
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
                    key_byte(master, c);
                    if (g_verbose)
                        printf("wmterm: KEY 0x%x view=%d\n",
                               (unsigned)c, g_view_offset);
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
            if (!row) continue;
            for (int c = 0; c < COLS; c++) {
                char ch = row[c];
                if (!ch) continue;
                gfx_glyph(&sctx, GRID_X + c * CELL_W,
                          GRID_Y + r * LINE_H, ch,
                          0xC0E0C0u, GFX_TRANSPARENT);
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
