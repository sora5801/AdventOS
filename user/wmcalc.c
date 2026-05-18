/*
 * wmcalc.c — session 139 / Path C phase 32 calculator.
 *
 * Usage:  wmcalc [seconds]
 *
 *   [seconds]   lifetime cap; default 600 (10 min).  Ctrl-Q exits.
 *
 * A simple four-function calculator: 4x5 button grid + display.
 * Integer-only — libuser has no float printer yet, so '.' is a
 * no-op for now.  Two-register evaluation: a pending op + an
 * accumulator.  Equivalent to feeding "1 + 2 + 3 =" through a
 * pocket calculator.
 *
 * Keys (focused window only):
 *   0-9         digit
 *   +  -  * /   operator
 *   = or Enter  evaluate
 *   c or C      clear all
 *   Backspace   drop the last digit of the current entry
 *   _           toggle sign of current entry
 *   Ctrl-Q      quit
 *
 * Mouse: click any button.  Buttons briefly highlight when pressed.
 *
 * Error state: division by zero shows "ERR" in red until C is hit.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W    220
#define WIN_H    320
#define HDR_H    18
#define MARGIN    8
#define DISP_X    MARGIN
#define DISP_Y    (HDR_H + 6)
#define DISP_W    (WIN_W - 2*MARGIN)
#define DISP_H    44
#define BTN_W     48
#define BTN_H     44
#define BTN_GAP    4
#define GRID_X    MARGIN
#define GRID_Y    (DISP_Y + DISP_H + 6)
#define COLS       4
#define ROWS       5

struct btn {
    const char *label;
    char        kind;   /* 'd' digit, 'o' op, 'e' equals, 'c' clear,
                         * 'b' back, 's' sign, '.' dot (no-op) */
    char        ch;     /* digit char or operator char */
};

/* Button grid:
 *   Row 0:  C    +/-  <-   /
 *   Row 1:  7    8    9    *
 *   Row 2:  4    5    6    -
 *   Row 3:  1    2    3    +
 *   Row 4:  0    .    =    AC
 */
static const struct btn g_btns[20] = {
    {"C",   'c', 0},   {"+/-", 's', 0}, {"<-",  'b', 0}, {"/",   'o', '/'},
    {"7",   'd', '7'}, {"8",   'd', '8'}, {"9",   'd', '9'}, {"*",   'o', '*'},
    {"4",   'd', '4'}, {"5",   'd', '5'}, {"6",   'd', '6'}, {"-",   'o', '-'},
    {"1",   'd', '1'}, {"2",   'd', '2'}, {"3",   'd', '3'}, {"+",   'o', '+'},
    {"0",   'd', '0'}, {".",   '.', 0}, {"=",   'e', 0},   {"AC",  'c', 0},
};

/* State machine. */
static int g_acc;
static int g_cur;
static char      g_op;        /* 0 or one of + - * slash */
static int       g_entering;  /* 1 while building g_cur */
static int       g_err;       /* 1 on div-by-zero etc */
static int       g_pressed;   /* button idx of last press, -1 if none */
static int       g_pressed_ticks;

static int apply(int a, char op, int b) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/':
            if (b == 0) { g_err = 1; return 0; }
            return a / b;
    }
    return b;
}

static void press_digit(int d) {
    if (g_err) return;
    if (!g_entering) { g_cur = 0; g_entering = 1; }
    /* Cap so we don't overflow int32_t. 9 digits keeps headroom
     * for one more multiply against another 9-digit operand. */
    int lim = 99999999;
    if (g_cur > lim || g_cur < -lim) return;
    if (g_cur < 0) g_cur = g_cur * 10 - d;
    else           g_cur = g_cur * 10 + d;
}

static void press_op(char op) {
    if (g_err) return;
    if (g_op != 0 && g_entering) {
        g_acc = apply(g_acc, g_op, g_cur);
        g_cur = g_acc;
    } else {
        g_acc = g_cur;
    }
    g_op = op;
    g_entering = 0;
}

static void press_eq(void) {
    if (g_err) return;
    if (g_op != 0) {
        g_cur = apply(g_acc, g_op, g_cur);
    }
    g_acc = g_cur;
    g_op = 0;
    g_entering = 0;
}

static void press_clear(void) {
    g_acc = 0; g_cur = 0; g_op = 0; g_entering = 0; g_err = 0;
}

static void press_back(void) {
    if (g_err) return;
    if (g_entering) g_cur /= 10;
    else            { g_cur = 0; g_entering = 1; }
}

static void press_sign(void) {
    if (g_err) return;
    g_cur = -g_cur;
}

static void press_button(int idx) {
    if (idx < 0 || idx >= 20) return;
    const struct btn *b = &g_btns[idx];
    switch (b->kind) {
        case 'd': press_digit(b->ch - '0'); break;
        case 'o': press_op(b->ch); break;
        case 'e': press_eq(); break;
        case 'c': press_clear(); break;
        case 'b': press_back(); break;
        case 's': press_sign(); break;
        case '.': /* integer-only build, see header */ break;
    }
    g_pressed = idx;
    g_pressed_ticks = 6;
}

static int key_to_button(unsigned int k) {
    if (k >= '0' && k <= '9') {
        for (int i = 0; i < 20; i++)
            if (g_btns[i].kind == 'd' && g_btns[i].ch == (char)k) return i;
    }
    switch (k) {
        case '+':                       return 15;
        case '-':                       return 11;
        case '*':                       return 7;
        case '/':                       return 3;
        case '=': case '\r': case '\n': return 18;
        case 'c': case 'C':             return 0;
        case 0x08: case 0x7F:           return 2;
        case '.':                       return 17;
        case '_':                       return 1;
    }
    return -1;
}

/* Render `v` as signed decimal (or "ERR") into buf.  Returns chars. */
static int print_decimal(char *buf, int cap, int v) {
    if (cap <= 0) return 0;
    if (g_err) {
        const char *e = "ERR"; int n = 0;
        while (e[n] && n < cap - 1) { buf[n] = e[n]; n++; }
        buf[n] = 0;
        return n;
    }
    char t[24]; int n = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[n++] = '0';
    else while (v && n < (int)sizeof(t)) {
        t[n++] = '0' + (int)(v % 10);
        v /= 10;
    }
    int out = 0;
    if (neg && out < cap - 1) buf[out++] = '-';
    while (n-- > 0 && out < cap - 1) buf[out++] = t[n];
    buf[out] = 0;
    return out;
}

static int hit_button(int x, int y) {
    if (x < GRID_X || y < GRID_Y) return -1;
    int cx = x - GRID_X;
    int cy = y - GRID_Y;
    int col    = cx / (BTN_W + BTN_GAP);
    int col_in = cx % (BTN_W + BTN_GAP);
    int row    = cy / (BTN_H + BTN_GAP);
    int row_in = cy % (BTN_H + BTN_GAP);
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return -1;
    if (col_in >= BTN_W || row_in >= BTN_H) return -1;
    return row * COLS + col;
}

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
    int seconds = 600;
    if (argc >= 2) seconds = atoi(argv[1]);
    if (seconds <= 0) seconds = 600;

    struct wm_window win;
    if (wm_open(&win, "wmcalc", WIN_W, WIN_H) < 0) {
        printf("wmcalc: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmcalc: id=%u\n", win.id);

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    int has_focus  = 0;
    int closed     = 0;
    g_pressed      = -1;
    int total_ticks = seconds * 30;

    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_CLOSE:   closed = 1; break;
                case WM_EV_KEY: {
                    if (ev.keycode == 0x11) { closed = 1; break; }
                    int idx = key_to_button(ev.keycode);
                    if (idx >= 0) press_button(idx);
                    break;
                }
                case WM_EV_MOUSE_PRESS: {
                    int idx = hit_button(ev.x, ev.y);
                    if (idx >= 0) press_button(idx);
                    break;
                }
                default: break;
            }
        }

        /* ---- paint ---- */
        wm_clear(&win, 0x1A1A24u);

        wm_fill_rect(&win, 0, 0, WIN_W, HDR_H,
                     has_focus ? 0x4080E0u : 0x404040u);
        gfx_text(&sctx, 6, 5,
                 has_focus ? "wmcalc  Ctrl-Q quit" : "wmcalc",
                 GFX_WHITE, GFX_TRANSPARENT);

        /* Display. */
        wm_fill_rect(&win, DISP_X, DISP_Y, DISP_W, DISP_H, 0x081018u);
        gfx_rect    (&sctx, DISP_X, DISP_Y, DISP_W, DISP_H, 0x305060u);

        char buf[24];
        int n = print_decimal(buf, (int)sizeof(buf), g_cur);
        int text_w = n * 8;
        int tx = DISP_X + DISP_W - 8 - text_w;
        int ty = DISP_Y + (DISP_H - 8) / 2;
        gfx_text(&sctx, tx, ty, buf,
                 g_err ? 0xE04040u : 0xE0F0E0u, GFX_TRANSPARENT);

        if (g_op != 0 && !g_err) {
            char ind[2] = { g_op, 0 };
            gfx_text(&sctx, DISP_X + 6, DISP_Y + 4, ind,
                     0x80C0E0u, GFX_TRANSPARENT);
        }

        /* Buttons. */
        for (int i = 0; i < 20; i++) {
            int col = i % COLS;
            int row = i / COLS;
            int bx = GRID_X + col * (BTN_W + BTN_GAP);
            int by = GRID_Y + row * (BTN_H + BTN_GAP);
            unsigned int bg;
            char k = g_btns[i].kind;
            if      (k == 'd' || k == '.') bg = 0x404858u;
            else if (k == 'o')             bg = 0x506070u;
            else if (k == 'e')             bg = 0x60A040u;
            else if (k == 'c')             bg = 0xA04040u;
            else                           bg = 0x504838u;
            if (i == g_pressed && g_pressed_ticks > 0) {
                bg = ((bg & 0xFEFEFEu) >> 1) + 0x808080u;
            }
            wm_fill_rect(&win, bx, by, BTN_W, BTN_H, bg);
            gfx_rect    (&sctx, bx, by, BTN_W, BTN_H, 0x202020u);

            const char *lbl = g_btns[i].label;
            int lw = 0;
            while (lbl[lw]) lw++;
            int gx = bx + (BTN_W - lw * 8) / 2;
            int gy = by + (BTN_H - 8) / 2;
            gfx_text(&sctx, gx, gy, lbl, 0xFFFFFFu, GFX_TRANSPARENT);
        }
        if (g_pressed_ticks > 0) g_pressed_ticks--;
        else                     g_pressed = -1;

        wm_present(&win);
        sys_sleep_ms(33);
    }

    wm_close(&win);
    return 0;
}
