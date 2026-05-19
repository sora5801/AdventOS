/*
 * wmcalc.c — session 139 / Path C phase 32 calculator,
 * extended in session 156 with decimal input and a memory
 * register.
 *
 * Usage:  wmcalc [seconds]
 *
 *   [seconds]   lifetime cap; default 600 (10 min).  Ctrl-Q exits.
 *
 * Now a four-function fixed-point calculator: 4x6 button grid
 * + display.  Each value is stored as (mantissa, decimals) so
 * "12.34" → mantissa 1234 + 2 decimals.  Arithmetic aligns the
 * decimal points before adding/subtracting, sums them for
 * multiplies, scales up the numerator for divides.  Overflow
 * detection on multiply lights the ERR display in red.
 *
 * Keys (focused window only):
 *   0-9         digit
 *   .           insert decimal point
 *   +  -  * /   operator
 *   = or Enter  evaluate
 *   c or C      clear all (including memory)
 *   Backspace   drop the last digit of the current entry
 *   _           toggle sign of current entry
 *   m           M+ (add current to memory)
 *   n           M- (subtract current from memory)
 *   r           MR (recall memory to current)
 *   k           MC (clear memory)
 *   Ctrl-Q      quit
 *
 * Mouse: click any button.  Buttons briefly highlight on press.
 *
 * Error state: divide-by-zero, overflow, or pattern-too-wide
 * shows "ERR" in red until C clears.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W    220
#define WIN_H    368                /* +48 for the new memory row */
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
#define ROWS       6                /* was 5; +1 for memory row */

struct btn {
    const char *label;
    char        kind;   /* 'd' digit, 'o' op, 'e' equals, 'c' clear,
                         * 'b' back, 's' sign, '.' dot,
                         * 'M' M+, 'N' M-, 'R' MR, 'K' MC */
    char        ch;
};

/*  Button grid (6 rows × 4 cols = 24):
 *    Row 0:  C    +/-  <-   /
 *    Row 1:  7    8    9    *
 *    Row 2:  4    5    6    -
 *    Row 3:  1    2    3    +
 *    Row 4:  0    .    =    AC
 *    Row 5:  M+   M-   MR   MC      ← session 156
 */
static const struct btn g_btns[24] = {
    {"C",   'c', 0},   {"+/-", 's', 0}, {"<-",  'b', 0}, {"/",   'o', '/'},
    {"7",   'd', '7'}, {"8",   'd', '8'}, {"9",   'd', '9'}, {"*",   'o', '*'},
    {"4",   'd', '4'}, {"5",   'd', '5'}, {"6",   'd', '6'}, {"-",   'o', '-'},
    {"1",   'd', '1'}, {"2",   'd', '2'}, {"3",   'd', '3'}, {"+",   'o', '+'},
    {"0",   'd', '0'}, {".",   '.', 0},   {"=",   'e', 0},   {"AC",  'c', 0},
    {"M+",  'M', 0},   {"M-",  'N', 0},   {"MR",  'R', 0},   {"MC",  'K', 0},
};

/* A signed fixed-point number: value == mant × 10^(-dec). */
struct num_t {
    int mant;
    int dec;
};

/* State. */
static struct num_t g_acc;
static struct num_t g_cur;
static struct num_t g_mem;
static char         g_op;
static int          g_entering;
static int          g_entering_dec;   /* 1 = decimals being typed */
static int          g_err;
static int          g_pressed;
static int          g_pressed_ticks;

/* Multiply two ints with overflow detection.  Returns 1 on
 * overflow (the kernel doesn't link libgcc so we lack the
 * builtin checked-arith helpers). */
static int safe_mul(int a, int b, int *out) {
    if (a == 0 || b == 0) { *out = 0; return 0; }
    int p = a * b;
    if (p / a != b) return 1;
    *out = p;
    return 0;
}

/* Normalise trailing zeros to keep dec from creeping upward
 * after operations like 1.500 -> 1.5. */
static void num_normalize(struct num_t *n) {
    while (n->dec > 0 && n->mant != 0 && n->mant % 10 == 0) {
        n->mant /= 10;
        n->dec--;
    }
}

/* Align src's decimal exponent up to target_dec by scaling
 * mant.  Caller has verified target_dec >= src->dec.  No overflow
 * check — for our tiny calculator the scaling factor is small. */
static int num_align(struct num_t *src, int target_dec) {
    while (src->dec < target_dec) {
        int m;
        if (safe_mul(src->mant, 10, &m)) return 1;
        src->mant = m;
        src->dec++;
    }
    return 0;
}

static struct num_t num_add(struct num_t a, struct num_t b, int sign) {
    int d = a.dec > b.dec ? a.dec : b.dec;
    if (num_align(&a, d) || num_align(&b, d)) {
        g_err = 1;
        return (struct num_t){0, 0};
    }
    struct num_t r = { sign > 0 ? a.mant + b.mant : a.mant - b.mant, d };
    num_normalize(&r);
    return r;
}

static struct num_t num_mul(struct num_t a, struct num_t b) {
    int m;
    if (safe_mul(a.mant, b.mant, &m)) { g_err = 1; return (struct num_t){0, 0}; }
    struct num_t r = { m, a.dec + b.dec };
    num_normalize(&r);
    return r;
}

static struct num_t num_div(struct num_t a, struct num_t b) {
    if (b.mant == 0) { g_err = 1; return (struct num_t){0, 0}; }
    /* Scale numerator up by 10^extra so the int divide preserves
     * fractional digits.  Try 4 extra; back off on overflow. */
    int extra = 0;
    int scaled = a.mant;
    while (extra < 4) {
        int s;
        if (safe_mul(scaled, 10, &s)) break;
        scaled = s;
        extra++;
    }
    struct num_t r = { scaled / b.mant, a.dec - b.dec + extra };
    while (r.dec < 0) {
        int m;
        if (safe_mul(r.mant, 10, &m)) { g_err = 1; return (struct num_t){0, 0}; }
        r.mant = m;
        r.dec++;
    }
    num_normalize(&r);
    return r;
}

static struct num_t apply(struct num_t a, char op, struct num_t b) {
    switch (op) {
        case '+': return num_add(a, b, +1);
        case '-': return num_add(a, b, -1);
        case '*': return num_mul(a, b);
        case '/': return num_div(a, b);
    }
    return b;
}

/* Digit cap so mant * 10 doesn't overflow int32.  9 digits keeps
 * mant under 10^9; one more multiply against a 9-digit operand
 * still fits. */
static int can_append_digit(int mant) {
    int abs_mant = mant < 0 ? -mant : mant;
    return abs_mant < 100000000;  /* room for one more 0..9 */
}

static void press_digit(int d) {
    if (g_err) return;
    if (!g_entering) {
        g_cur = (struct num_t){0, 0};
        g_entering = 1;
        g_entering_dec = 0;
    }
    if (!can_append_digit(g_cur.mant)) return;
    if (g_cur.mant < 0) g_cur.mant = g_cur.mant * 10 - d;
    else                g_cur.mant = g_cur.mant * 10 + d;
    if (g_entering_dec) g_cur.dec++;
}

static void press_dot(void) {
    if (g_err) return;
    if (!g_entering) {
        g_cur = (struct num_t){0, 0};
        g_entering = 1;
    }
    g_entering_dec = 1;
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
    g_entering_dec = 0;
}

static void press_eq(void) {
    if (g_err) return;
    if (g_op != 0) {
        g_cur = apply(g_acc, g_op, g_cur);
    }
    g_acc = g_cur;
    g_op = 0;
    g_entering = 0;
    g_entering_dec = 0;
}

static void press_clear(void) {
    g_acc = (struct num_t){0, 0};
    g_cur = (struct num_t){0, 0};
    g_mem = (struct num_t){0, 0};
    g_op = 0;
    g_entering = 0;
    g_entering_dec = 0;
    g_err = 0;
}

static void press_back(void) {
    if (g_err) return;
    if (g_entering) {
        if (g_entering_dec && g_cur.dec > 0) {
            /* drop a decimal digit */
            g_cur.mant /= 10;
            g_cur.dec--;
        } else if (g_entering_dec) {
            /* leaving decimal mode without removing any int digit */
            g_entering_dec = 0;
        } else {
            g_cur.mant /= 10;
        }
    } else {
        g_cur = (struct num_t){0, 0};
        g_entering = 1;
        g_entering_dec = 0;
    }
}

static void press_sign(void) {
    if (g_err) return;
    g_cur.mant = -g_cur.mant;
}

static void press_mem(char which) {
    if (g_err && which != 'K') return;
    switch (which) {
        case 'M': g_mem = num_add(g_mem, g_cur, +1); break;
        case 'N': g_mem = num_add(g_mem, g_cur, -1); break;
        case 'R': g_cur = g_mem; g_entering = 0; g_entering_dec = 0; break;
        case 'K': g_mem = (struct num_t){0, 0}; break;
    }
}

static void press_button(int idx) {
    if (idx < 0 || idx >= 24) return;
    const struct btn *b = &g_btns[idx];
    switch (b->kind) {
        case 'd': press_digit(b->ch - '0'); break;
        case 'o': press_op(b->ch); break;
        case 'e': press_eq(); break;
        case 'c': press_clear(); break;
        case 'b': press_back(); break;
        case 's': press_sign(); break;
        case '.': press_dot(); break;
        case 'M': case 'N': case 'R': case 'K':
            press_mem(b->kind); break;
    }
    g_pressed = idx;
    g_pressed_ticks = 6;
}

static int key_to_button(unsigned int k) {
    if (k >= '0' && k <= '9') {
        for (int i = 0; i < 24; i++)
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
        /* Session 156 — memory shortcuts. */
        case 'm':                       return 20;
        case 'n':                       return 21;
        case 'r':                       return 22;
        case 'k':                       return 23;
    }
    return -1;
}

/* Render a num_t into buf as signed decimal.  Returns chars. */
static int print_num(char *buf, int cap, struct num_t n) {
    if (cap <= 0) return 0;
    if (g_err) {
        const char *e = "ERR"; int i = 0;
        while (e[i] && i < cap - 1) { buf[i] = e[i]; i++; }
        buf[i] = 0; return i;
    }
    int v = n.mant;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }

    /* Split into int and fractional parts. */
    int divisor = 1;
    for (int i = 0; i < n.dec; i++) divisor *= 10;
    int int_part = (divisor > 0) ? (v / divisor) : v;
    int frac_part = (divisor > 0) ? (v - int_part * divisor) : 0;

    int out = 0;
    if (neg && out < cap - 1) buf[out++] = '-';

    /* Print integer part (always at least one digit). */
    if (int_part == 0) {
        if (out < cap - 1) buf[out++] = '0';
    } else {
        char ip[12]; int ipn = 0;
        while (int_part) { ip[ipn++] = '0' + int_part % 10; int_part /= 10; }
        while (ipn-- > 0) if (out < cap - 1) buf[out++] = ip[ipn];
    }

    /* Optional fractional part. */
    if (n.dec > 0) {
        if (out < cap - 1) buf[out++] = '.';
        char fp[12]; int fpn = 0;
        int t = frac_part;
        while (t) { fp[fpn++] = '0' + t % 10; t /= 10; }
        /* Leading zeros to fill `dec` width. */
        while (fpn < n.dec) fp[fpn++] = '0';
        while (fpn-- > 0) if (out < cap - 1) buf[out++] = fp[fpn];
    }
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
        int n = print_num(buf, (int)sizeof(buf), g_cur);
        int text_w = n * 8;
        int tx = DISP_X + DISP_W - 8 - text_w;
        int ty = DISP_Y + (DISP_H - 8) / 2;
        gfx_text(&sctx, tx, ty, buf,
                 g_err ? 0xE04040u : 0xE0F0E0u, GFX_TRANSPARENT);

        /* Top-left indicator: pending operator. */
        if (g_op != 0 && !g_err) {
            char ind[2] = { g_op, 0 };
            gfx_text(&sctx, DISP_X + 6, DISP_Y + 4, ind,
                     0x80C0E0u, GFX_TRANSPARENT);
        }
        /* Bottom-left indicator: "M" when memory is non-zero. */
        if (g_mem.mant != 0 && !g_err) {
            gfx_text(&sctx, DISP_X + 6, DISP_Y + DISP_H - 12, "M",
                     0xE0A040u, GFX_TRANSPARENT);
        }

        /* Buttons. */
        for (int i = 0; i < 24; i++) {
            int col = i % COLS;
            int row = i / COLS;
            int bx = GRID_X + col * (BTN_W + BTN_GAP);
            int by = GRID_Y + row * (BTN_H + BTN_GAP);
            unsigned int bg;
            char k = g_btns[i].kind;
            if      (k == 'd' || k == '.')              bg = 0x404858u;
            else if (k == 'o')                          bg = 0x506070u;
            else if (k == 'e')                          bg = 0x60A040u;
            else if (k == 'c')                          bg = 0xA04040u;
            else if (k == 'M' || k == 'N' || k == 'R'
                  || k == 'K')                          bg = 0x405068u; /* memory row */
            else                                        bg = 0x504838u;
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
