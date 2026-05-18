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
#define WIN_H        240    /* 240 = 24 * 10 (line h) + 4 px margin */
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

static void grid_clear(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            g_grid[r][c] = 0;
    g_cur_col = 0;
    g_cur_row = 0;
}

/* Shift every row up by one; bottom row becomes blank. */
static void grid_scroll(void) {
    for (int r = 0; r < ROWS - 1; r++)
        for (int c = 0; c < COLS; c++)
            g_grid[r][c] = g_grid[r+1][c];
    for (int c = 0; c < COLS; c++) g_grid[ROWS-1][c] = 0;
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

/* Feed one byte through the VT-100ish state machine. */
static void vt_feed(unsigned char b) {
    if (g_vt_state == 1) {
        if (b == '[') { g_vt_state = 2; return; }
        g_vt_state = 0;
        return;
    }
    if (g_vt_state == 2) {
        /* ESC [ ... <final> — final is in 0x40..0x7E.  Strip the
         * whole sequence. */
        if (b >= 0x40 && b <= 0x7E) g_vt_state = 0;
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

int main(int argc, char **argv) {
    int seconds = 120;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 120;

    struct wm_window win;
    if (wm_open(&win, "wmterm", WIN_W, WIN_H) < 0) {
        printf("wmterm: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmterm: id=%u\n", win.id);

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
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_CLOSE:   closed = 1; break;
                case WM_EV_KEY: {
                    char c = (char)ev.keycode;
                    sys_write(master, &c, 1);
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
            for (int i = 0; i < n; i++) vt_feed((unsigned char)buf[i]);
        }

        /* Paint. */
        wm_clear(&win, 0x080808u);
        wm_fill_rect(&win, 0, 0, WIN_W, HDR_H,
                     has_focus ? 0x4080E0u : 0x404040u);
        gfx_text(&sctx, 6, 6,
                 has_focus ? "wmterm - sh.elf"
                           : "wmterm - sh.elf (click to focus)",
                 GFX_WHITE, GFX_TRANSPARENT);

        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                char ch = g_grid[r][c];
                if (!ch) continue;
                gfx_glyph(&sctx, GRID_X + c * CELL_W,
                          GRID_Y + r * LINE_H, ch,
                          0xC0E0C0u, GFX_TRANSPARENT);
            }
        }

        /* Blinking caret. */
        if (has_focus && ((caret_phase / 12) & 1) == 0) {
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
