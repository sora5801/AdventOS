/*
 * gui — userspace window manager + built-in apps (session 57).
 *
 * Replaces the session-34 single-window demo. The new design is a
 * proper compositing WM:
 *
 *   * a small fixed window table (struct window[MAX_WINDOWS])
 *   * each window has (x, y, w, h, z, focused, alive) + a per-app
 *     draw/on_click/on_key callback set
 *   * an event loop at ~60 fps that polls the mouse + keyboard,
 *     hit-tests against the topmost window, dispatches click /
 *     drag / key events
 *   * a redraw pass per frame: clear the desktop, sort windows by
 *     z, paint each window's frame + content, paint the cursor on
 *     top. No partial-redraw / damage tracking — at 1024x768/32bpp
 *     a full screen blit is ~3 MiB and QEMU eats it without flinching.
 *
 * Four built-in apps demonstrate the model:
 *
 *   * "Hello"   - static text window
 *   * "Clock"   - HH:MM:SS, redraws every frame from sys_time()
 *   * "Paint"   - 64x48 pixel canvas, drag with mouse to paint
 *   * "Tasks"   - live process list: reads /proc/, shows pid + name
 *
 * The WM also accepts arg `selftest` to run a deterministic scripted
 * demo (move windows by injected mouse events, take a screenshot via
 * pixel reads, verify, exit). t39 drives this.
 *
 * Coexistence with kernel fbcon: as soon as we mmap the framebuffer
 * we call sys_fb_takeover(1) to freeze kernel writes, so kprintf-style
 * output during our run can't tear into our rendering. On exit we
 * release with sys_fb_takeover(0).
 */

#include "libuser.h"
#include "../kernel/font8x8.h"

/* ---- Framebuffer geometry + primitives -------------------------- */

static volatile unsigned char *g_fb;
static unsigned int g_w, g_h, g_bpp, g_pitch;

static inline void put_pixel(int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || (unsigned)x >= g_w || (unsigned)y >= g_h) return;
    volatile unsigned char *row = g_fb + (unsigned)y * g_pitch;
    if (g_bpp == 32) {
        ((volatile uint32_t *)row)[x] = rgb;
    } else if (g_bpp == 24) {
        volatile unsigned char *p = row + x * 3;
        p[0] = (unsigned char)(rgb);          /* B */
        p[1] = (unsigned char)(rgb >> 8);     /* G */
        p[2] = (unsigned char)(rgb >> 16);    /* R */
    } else { /* 16 bpp R5 G6 B5 */
        unsigned int r = (rgb >> 16) & 0xFF;
        unsigned int g = (rgb >>  8) & 0xFF;
        unsigned int b =  rgb        & 0xFF;
        unsigned int v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        ((volatile uint16_t *)row)[x] = (uint16_t)v;
    }
}

/* Read one pixel back — needed by the selftest to verify rendering.
 * Returns 0 on out-of-range (vs a defined sentinel; the test compares
 * exact known colors). */
static unsigned int get_pixel(int x, int y) {
    if (x < 0 || y < 0 || (unsigned)x >= g_w || (unsigned)y >= g_h) return 0;
    volatile unsigned char *row = g_fb + (unsigned)y * g_pitch;
    if (g_bpp == 32) return ((volatile uint32_t *)row)[x] & 0xFFFFFFu;
    if (g_bpp == 24) {
        volatile unsigned char *p = row + x * 3;
        return ((unsigned int)p[2] << 16) | ((unsigned int)p[1] << 8) | p[0];
    }
    unsigned int v = ((volatile uint16_t *)row)[x];
    unsigned int r = (v >> 11) & 0x1F;
    unsigned int g = (v >>  5) & 0x3F;
    unsigned int b =  v        & 0x1F;
    return ((r << 3) << 16) | ((g << 2) << 8) | (b << 3);
}

static void fill_rect(int x0, int y0, int w, int h, unsigned int rgb) {
    if (w <= 0 || h <= 0) return;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > (int)g_w) w = (int)g_w - x0;
    if (y0 + h > (int)g_h) h = (int)g_h - y0;
    if (w <= 0 || h <= 0) return;

    /* For 32-bpp we punch out rows with a tight inner loop — the
     * naive nested put_pixel was the hot path of the old gui demo
     * at 60 fps and bottlenecked everything else. */
    if (g_bpp == 32) {
        for (int y = 0; y < h; y++) {
            volatile uint32_t *row = (volatile uint32_t *)
                (g_fb + (unsigned)(y0 + y) * g_pitch + (unsigned)x0 * 4);
            for (int x = 0; x < w; x++) row[x] = rgb;
        }
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                put_pixel(x0 + x, y0 + y, rgb);
    }
}

static void rect_outline(int x0, int y0, int w, int h, unsigned int rgb) {
    for (int x = 0; x < w; x++) {
        put_pixel(x0 + x,         y0,             rgb);
        put_pixel(x0 + x,         y0 + h - 1,     rgb);
    }
    for (int y = 0; y < h; y++) {
        put_pixel(x0,             y0 + y,         rgb);
        put_pixel(x0 + w - 1,     y0 + y,         rgb);
    }
}

/* Draw one glyph from the kernel-supplied font8x8 table. */
static void draw_glyph(int x, int y, char c, unsigned int rgb) {
    if ((uint8_t)c < FONT_FIRST_CH || (uint8_t)c > FONT_LAST_CH) return;
    const uint8_t *glyph = font8x8[(uint8_t)c - FONT_FIRST_CH];
    for (int r = 0; r < FONT_H; r++) {
        uint8_t bits = glyph[r];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (1u << col)) put_pixel(x + col, y + r, rgb);
        }
    }
}

static void draw_str(int x, int y, const char *s, unsigned int rgb) {
    for (; *s; s++) {
        if (*s == '\n') { y += FONT_H; continue; }
        draw_glyph(x, y, *s, rgb);
        x += FONT_W;
    }
}

static void draw_int(int x, int y, int n, unsigned int rgb) {
    char buf[12]; int i = 0;
    int neg = n < 0;
    if (neg) n = -n;
    if (n == 0) buf[i++] = '0';
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    if (neg) buf[i++] = '-';
    while (i--) { draw_glyph(x, y, buf[i], rgb); x += FONT_W; }
}

/* ---- Cursor sprite ---------------------------------------------- */

static const unsigned short cursor_outline[12] = {
    0x0001,0x0003,0x0007,0x000F,0x001F,0x003F,
    0x007F,0x00FF,0x01FF,0x03FF,0x00DB,0x0099,
};
static const unsigned short cursor_fill[12] = {
    0x0000,0x0001,0x0003,0x0007,0x000F,0x001F,
    0x003F,0x007F,0x00FF,0x01FF,0x004A,0x0028,
};
static void draw_cursor(int x, int y) {
    for (int r = 0; r < 12; r++) {
        unsigned short ob = cursor_outline[r];
        unsigned short fb = cursor_fill[r];
        for (int c = 0; c < 12; c++) {
            if (ob & (1 << c)) put_pixel(x + c, y + r, 0x000000);
            if (fb & (1 << c)) put_pixel(x + c, y + r, 0xFFFFFF);
        }
    }
}

/* ---- Window model ------------------------------------------------ */

#define MAX_WINDOWS  8
#define TITLE_H      18
#define CLOSE_W      14
#define BORDER       1

struct window;
typedef void (*draw_fn)    (struct window *, int frame);
typedef void (*click_fn)   (struct window *, int lx, int ly, int btns);
typedef void (*key_fn)     (struct window *, int key);

struct window {
    int      alive;
    int      id;
    char     title[32];
    int      x, y;         /* top-left, including title bar */
    int      w, h;         /* total, including title bar */
    int      z;            /* higher = nearer the front */
    int      focused;
    draw_fn  draw;
    click_fn click;
    key_fn   key;
    /* Session 61: distinguish "button-style" apps from "drag-style".
     * Button-style apps (Calc, Notepad) only want a click event on
     * the down-edge — repeated dispatches while the button is held
     * would re-fire the action every frame.  Drag-style apps (Paint)
     * explicitly opt in so the canvas can keep painting under a held
     * mouse.  Default 0 = button-style. */
    int      wants_drag;
    int      state[16];    /* per-app scratchpad */
};

static struct window g_wins[MAX_WINDOWS];
static int g_next_z = 1;

static int win_alloc(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_wins[i].alive) {
            for (size_t b = 0; b < sizeof(g_wins[i]); b++)
                ((uint8_t *)&g_wins[i])[b] = 0;
            g_wins[i].alive = 1;
            g_wins[i].id    = i;
            return i;
        }
    }
    return -1;
}

/* Pick the topmost focused window. Returns -1 if no windows are alive. */
static int win_focused_idx(void) {
    int best = -1, best_z = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_wins[i].alive)      continue;
        if (g_wins[i].z <= best_z) continue;
        best   = i;
        best_z = g_wins[i].z;
    }
    return best;
}

static void win_raise(int idx) {
    /* Bring `idx` to the top by giving it the highest z. */
    if (idx < 0 || idx >= MAX_WINDOWS || !g_wins[idx].alive) return;
    g_wins[idx].z = ++g_next_z;
    for (int i = 0; i < MAX_WINDOWS; i++) g_wins[i].focused = 0;
    g_wins[idx].focused = 1;
}

/* Topmost alive window containing (px, py), or -1. */
static int win_hit_test(int px, int py) {
    int best = -1, best_z = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *w = &g_wins[i];
        if (!w->alive) continue;
        if (px < w->x || px >= w->x + w->w) continue;
        if (py < w->y || py >= w->y + w->h) continue;
        if (w->z > best_z) { best = i; best_z = w->z; }
    }
    return best;
}

/* ---- Window chrome (title bar + frame + close-X) ----------------- */

static void draw_window_frame(struct window *w) {
    /* Title bar color depends on focus. */
    unsigned int title_bg = w->focused ? 0x305080u : 0x404040u;
    unsigned int title_fg = 0xFFFFFFu;
    unsigned int body_bg  = 0xE0E0E0u;
    unsigned int border   = 0x101010u;

    /* Outer border. */
    rect_outline(w->x, w->y, w->w, w->h, border);

    /* Title bar. */
    fill_rect(w->x + 1, w->y + 1, w->w - 2, TITLE_H - 1, title_bg);

    /* Title text — clipped if the title bar is too narrow. */
    int title_x = w->x + 6;
    int title_y = w->y + (TITLE_H - FONT_H) / 2;
    int max_chars = (w->w - CLOSE_W - 12) / FONT_W;
    for (int i = 0; i < max_chars && w->title[i]; i++) {
        draw_glyph(title_x + i * FONT_W, title_y, w->title[i], title_fg);
    }

    /* Close button: a black X on a 14x14 darker square at the right
     * edge of the title bar. Click hit-test in handle_click(). */
    int cx = w->x + w->w - CLOSE_W - 3;
    int cy = w->y + 2;
    fill_rect(cx, cy, CLOSE_W, TITLE_H - 4, 0xC03030u);
    for (int i = 3; i < CLOSE_W - 3; i++) {
        put_pixel(cx + i,           cy + i,           0xFFFFFFu);
        put_pixel(cx + CLOSE_W - 1 - i, cy + i,       0xFFFFFFu);
    }

    /* Content body — fill below the title bar, leaving 1-px border. */
    fill_rect(w->x + 1, w->y + TITLE_H,
              w->w - 2, w->h - TITLE_H - 1, body_bg);
}

/* (lx, ly) are client-local coords, with (0, 0) at the body top-left
 * (BELOW the title bar). Returns "did we eat the click for chrome?"
 * by handling drag / close. The actual app's on_click gets the
 * adjusted coords if we don't eat it. */
static int g_drag_idx = -1;
static int g_drag_dx, g_drag_dy;

static int hit_close_button(struct window *w, int px, int py) {
    int cx = w->x + w->w - CLOSE_W - 3;
    int cy = w->y + 2;
    return (px >= cx && px < cx + CLOSE_W &&
            py >= cy && py < cy + TITLE_H - 4);
}
static int hit_title_bar(struct window *w, int px, int py) {
    return (px >= w->x + 1 && px < w->x + w->w - CLOSE_W - 3 &&
            py >= w->y + 1 && py < w->y + TITLE_H);
}

/* ---- Text-field widget (session 61) -----------------------------
 *
 * A reusable single-line edit primitive. The buffer is caller-owned
 * (passed in at init), so the same widget code drives both the
 * Calculator's 64-byte expression line and the Notepad's 1024-byte
 * scrollback. The widget tracks:
 *
 *   - len:    current valid bytes (0..cap-1)
 *   - cap:    total buffer size (so the NUL terminator lives at
 *             buf[len] and never overflows)
 *
 * Editing is "append-only with backspace" — no caret-position cursor
 * movement.  Arrow keys would require parsing escape sequences out
 * of the keyboard ring (which sys_kbd_poll surfaces a byte at a time)
 * and the WM's top-level loop currently treats a bare ESC as
 * "quit"; we deferred that complexity.  For a calculator and a
 * notebook-jot-pad, append + backspace covers the actual use case.
 *
 * Rendering: a 1-px border (blue when focused, grey when not), text
 * in a fixed-pitch glyph stream, and a blinking `_` cursor at the
 * end of the text — but only when the widget is focused, so the
 * unfocused windows on the desktop don't strobe in unison. */

#define TF_MAX 1024

struct text_field {
    char *buf;          /* not owned; caller supplies storage */
    int   cap;          /* sizeof(buf) — buf[cap-1] is the NUL slot */
    int   len;          /* current string length, not counting NUL */
};

static void tf_init(struct text_field *tf, char *buf, int cap) {
    tf->buf = buf;
    tf->cap = cap;
    tf->len = 0;
    if (cap > 0) buf[0] = 0;
}

static void tf_clear(struct text_field *tf) {
    tf->len = 0;
    if (tf->cap > 0) tf->buf[0] = 0;
}

static void tf_append(struct text_field *tf, char c) {
    if (tf->len >= tf->cap - 1) return;     /* leave room for NUL */
    tf->buf[tf->len++] = c;
    tf->buf[tf->len]    = 0;
}

static void tf_backspace(struct text_field *tf) {
    if (tf->len == 0) return;
    tf->len--;
    tf->buf[tf->len] = 0;
}

/* Returns 1 if the key was ENTER (so callers can submit), 0 otherwise.
 * Backspace mappings: 0x08 (Ctrl-H, what raw mode usually emits) and
 * 0x7F (DEL, what some terminals send) both delete the previous char.
 * Newline accepts both LF (0x0A) and CR (0x0D) for cross-platform
 * convenience. */
static int tf_handle_key(struct text_field *tf, int key) {
    if (key == '\n' || key == '\r')      return 1;
    if (key == 0x08 || key == 0x7F) { tf_backspace(tf); return 0; }
    if (key >= 0x20 && key < 0x7F) { tf_append(tf, (char)key); return 0; }
    return 0;
}

/* Render the field at (x, y) with the given content size (w, h).
 * `focused` controls the border color + cursor visibility; `frame`
 * is the WM's frame counter, used to blink the cursor at ~2 Hz. */
static void tf_draw(const struct text_field *tf,
                    int x, int y, int w, int h,
                    int focused, int frame) {
    uint32_t bg     = 0xFFFFFFu;
    uint32_t border = focused ? 0x4080E0u : 0x808080u;
    uint32_t fg     = 0x101010u;

    fill_rect(x, y, w, h, bg);
    rect_outline(x, y, w, h, border);
    if (focused) {
        /* Double-thickness border = clearer focus cue without needing
         * a separate "focused-bg" color that'd clash on dark themes. */
        rect_outline(x + 1, y + 1, w - 2, h - 2, border);
    }

    /* Glyphs are 8x8 monospace.  If the text outruns the visible
     * width, scroll the right edge into view by chopping from the
     * left so the cursor stays on-screen. */
    int char_cap = (w - 8) / FONT_W;
    if (char_cap < 1) char_cap = 1;
    int start = 0;
    if (tf->len + 1 > char_cap) {
        start = tf->len + 1 - char_cap;
    }

    int tx = x + 4;
    int ty = y + (h - FONT_H) / 2;
    for (int i = start; i < tf->len; i++) {
        draw_glyph(tx + (i - start) * FONT_W, ty, tf->buf[i], fg);
    }

    int blink_on = (frame / 15) & 1;            /* ~2 Hz at 60 fps */
    if (focused && blink_on) {
        int cx = tx + (tf->len - start) * FONT_W;
        draw_glyph(cx, ty, '_', fg);
    }
}

/* ---- Apps -------------------------------------------------------- */

/* "Hello" — three lines of static text. */
static void hello_draw(struct window *w, int frame) {
    (void)frame;
    int bx = w->x + 8;
    int by = w->y + TITLE_H + 8;
    draw_str(bx, by,      "AdventOS GUI / session 57", 0x202020u);
    draw_str(bx, by + 14, "click to focus, drag", 0x505050u);
    draw_str(bx, by + 28, "title bar, X to close.", 0x505050u);
}

/* "Clock" — HH:MM:SS from sys_time(), updated every frame. */
static void clock_draw(struct window *w, int frame) {
    (void)frame;
    int t = (int)sys_time();
    int s =  t       % 60;
    int m = (t / 60) % 60;
    int h = (t / 3600) % 24;
    int bx = w->x + 12;
    int by = w->y + TITLE_H + 14;
    char buf[16];
    int i = 0;
    buf[i++] = '0' + (h / 10); buf[i++] = '0' + (h % 10); buf[i++] = ':';
    buf[i++] = '0' + (m / 10); buf[i++] = '0' + (m % 10); buf[i++] = ':';
    buf[i++] = '0' + (s / 10); buf[i++] = '0' + (s % 10); buf[i++] = 0;
    /* Draw it twice — once as a "shadow", once on top — for a chunky
     * digital-clock look without doing a bigger font. */
    draw_str(bx + 1, by + 1, buf, 0xC0C0C0u);
    draw_str(bx,     by,     buf, 0x101040u);
    draw_str(bx, by + 24,    "unix seconds since boot:", 0x707070u);
    draw_int(bx, by + 38, t,                            0x707070u);
}

/* "Paint" — 64x48 canvas at 4 px/cell. state[0..MAX_C-1] = packed
 * RGB cells; state has only 16 ints so we use a separate static. */
#define PAINT_W   64
#define PAINT_H   48
#define PAINT_CELL  4
static uint32_t g_paint_canvas[PAINT_W * PAINT_H];

static void paint_draw(struct window *w, int frame) {
    (void)frame;
    int bx = w->x + 8;
    int by = w->y + TITLE_H + 8;
    /* Canvas border for clarity. */
    rect_outline(bx - 1, by - 1,
                 PAINT_W * PAINT_CELL + 2, PAINT_H * PAINT_CELL + 2,
                 0x000000u);
    for (int j = 0; j < PAINT_H; j++) {
        for (int i = 0; i < PAINT_W; i++) {
            uint32_t c = g_paint_canvas[j * PAINT_W + i];
            if (c == 0) c = 0xFFFFFFu;
            fill_rect(bx + i * PAINT_CELL, by + j * PAINT_CELL,
                      PAINT_CELL, PAINT_CELL, c);
        }
    }
    draw_str(bx, by + PAINT_H * PAINT_CELL + 4,
             "drag with left btn to paint",
             0x404040u);
    /* Color swatches at right edge for click-to-pick. */
    static const uint32_t pal[6] = {
        0xE03030u, 0x30E030u, 0x4080E0u,
        0xE0E030u, 0xC030E0u, 0x000000u,
    };
    int sx = bx + PAINT_W * PAINT_CELL + 8;
    for (int i = 0; i < 6; i++) {
        fill_rect(sx, by + i * 18, 14, 14, pal[i]);
        if ((uint32_t)w->state[0] == pal[i]) {
            rect_outline(sx - 1, by + i * 18 - 1, 16, 16, 0x000000u);
        }
    }
}
static void paint_click(struct window *w, int lx, int ly, int btns) {
    int bx = 8, by = 8;            /* content offset within w */
    /* Color swatch column. */
    int sx = bx + PAINT_W * PAINT_CELL + 8;
    for (int i = 0; i < 6; i++) {
        if (lx >= sx && lx < sx + 14 &&
            ly >= by + i * 18 && ly < by + i * 18 + 14) {
            static const uint32_t pal[6] = {
                0xE03030u, 0x30E030u, 0x4080E0u,
                0xE0E030u, 0xC030E0u, 0x000000u,
            };
            w->state[0] = (int)pal[i];
            return;
        }
    }
    /* Canvas paint — only on left-button hold. */
    if (!(btns & 0x1)) return;
    int cx = (lx - bx) / PAINT_CELL;
    int cy = (ly - by) / PAINT_CELL;
    if (cx < 0 || cx >= PAINT_W || cy < 0 || cy >= PAINT_H) return;
    uint32_t color = w->state[0] ? (uint32_t)w->state[0] : 0xE03030u;
    g_paint_canvas[cy * PAINT_W + cx] = color;
}

/* "Tasks" — read /proc, list pid + state. */
static void tasks_draw(struct window *w, int frame) {
    (void)frame;
    int bx = w->x + 8;
    int by = w->y + TITLE_H + 8;
    draw_str(bx, by, " pid  name", 0x202020u);
    int row = 1;
    int iter = 0;
    char name[16];
    for (;;) {
        int idx = sys_readdir("/proc", &iter, name);
        if (idx < 0) break;
        /* /proc enumerates pid dirs. Skip non-digit names. */
        if (!(name[0] >= '0' && name[0] <= '9')) continue;
        char path[64];
        int n = 0;
        const char *p = "/proc/";
        while (*p) path[n++] = *p++;
        for (int i = 0; name[i] && n < 60; i++) path[n++] = name[i];
        const char *t = "/status";
        while (*t) path[n++] = *t++;
        path[n] = 0;
        int fd = sys_open(path);
        if (fd < 0) continue;
        char buf[128];
        int nb = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (nb <= 0) continue;
        buf[nb] = 0;
        /* /proc/N/status starts with "Name: <name>". */
        char proc_name[24] = {0};
        if (buf[0] == 'N' && buf[1] == 'a' && buf[2] == 'm' &&
            buf[3] == 'e' && buf[4] == ':') {
            int j = 6, k = 0;
            while (buf[j] == ' ') j++;
            while (buf[j] && buf[j] != '\n' && k < 23) proc_name[k++] = buf[j++];
        }
        int y = by + row * 11;
        if (y > w->y + w->h - 12) break;
        /* pid (4 digits right-justified) + name. */
        char pad[5] = "    ";
        int pl = 0; while (name[pl]) pl++;
        int pad_n = 4 - pl;
        if (pad_n < 0) pad_n = 0;
        draw_str(bx + pad_n * FONT_W, y, name, 0x202020u);
        draw_str(bx + 5 * FONT_W,     y, proc_name, 0x303030u);
        (void)pad;
        row++;
    }
}

/* ---- Calculator app (session 61) -------------------------------
 *
 * Single-line expression input, eval-on-Enter, button grid for mouse-
 * only operation, full keyboard support too.  The evaluator is a tiny
 * recursive-descent parser:
 *
 *   expr   := term  (('+' | '-') term)*
 *   term   := factor (('*' | '/') factor)*
 *   factor := number | '-' factor | '(' expr ')'
 *
 * Integer arithmetic only — adding decimals would mean dragging in a
 * floating-point representation, which is currently out of scope on
 * this freestanding userland.  Division by zero returns 0 with the
 * error flag set; any unparsed trailing input also flags an error.
 *
 * On every evaluation the result is printed to stdout (visible to
 * the t44 selftest via captured fork pipes); the on-screen display
 * shows it as "= <n>" in the result box. */

static char  g_calc_buf[64];
static struct text_field g_calc_tf;
static int   g_calc_inited;
static int   g_calc_have_result;
static int   g_calc_result;
static int   g_calc_err;

struct calc_p {
    const char *p;
    int         err;
};
static void calc_skip_ws(struct calc_p *cp) {
    while (*cp->p == ' ' || *cp->p == '\t') cp->p++;
}
static int calc_expr(struct calc_p *cp);

static int calc_factor(struct calc_p *cp) {
    calc_skip_ws(cp);
    if (*cp->p == '(') {
        cp->p++;
        int v = calc_expr(cp);
        calc_skip_ws(cp);
        if (*cp->p != ')') { cp->err = 1; return 0; }
        cp->p++;
        return v;
    }
    int neg = 0;
    if (*cp->p == '-') { neg = 1; cp->p++; calc_skip_ws(cp); }
    if (!(*cp->p >= '0' && *cp->p <= '9')) { cp->err = 1; return 0; }
    int v = 0;
    while (*cp->p >= '0' && *cp->p <= '9') {
        v = v * 10 + (*cp->p - '0');
        cp->p++;
    }
    return neg ? -v : v;
}
static int calc_term(struct calc_p *cp) {
    int v = calc_factor(cp);
    for (;;) {
        calc_skip_ws(cp);
        if (*cp->p == '*') { cp->p++; v = v * calc_factor(cp); }
        else if (*cp->p == '/') {
            cp->p++;
            int r = calc_factor(cp);
            if (r == 0) { cp->err = 1; return 0; }
            v = v / r;
        }
        else break;
    }
    return v;
}
static int calc_expr(struct calc_p *cp) {
    int v = calc_term(cp);
    for (;;) {
        calc_skip_ws(cp);
        if (*cp->p == '+')      { cp->p++; v += calc_term(cp); }
        else if (*cp->p == '-') { cp->p++; v -= calc_term(cp); }
        else break;
    }
    return v;
}

static void calc_evaluate(void) {
    struct calc_p cp = { g_calc_tf.buf, 0 };
    int v = calc_expr(&cp);
    calc_skip_ws(&cp);
    if (cp.err || *cp.p != 0 || g_calc_tf.len == 0) {
        g_calc_err = 1;
        g_calc_have_result = 0;
        printf("calc: '%s' = ERROR\n", g_calc_tf.buf);
    } else {
        g_calc_result = v;
        g_calc_have_result = 1;
        g_calc_err = 0;
        printf("calc: '%s' = %d\n", g_calc_tf.buf, v);
    }
}

static void calc_lazy_init(void) {
    if (g_calc_inited) return;
    tf_init(&g_calc_tf, g_calc_buf, (int)sizeof(g_calc_buf));
    g_calc_inited = 1;
}

/* Button grid table — 4 columns, 4 rows of operators/digits + a
 * full-width "=" bar at the bottom. */
static const char g_calc_btn[4][4] = {
    { '7', '8', '9', '/' },
    { '4', '5', '6', '*' },
    { '1', '2', '3', '-' },
    { '0', '.', 'C', '+' },
};

/* Pixel offsets within the window content (relative to body origin,
 * i.e. (w->x + 0, w->y + TITLE_H)). Shared between draw + click so
 * hit-tests match render exactly. */
#define CALC_PAD          8
#define CALC_FIELD_H     22
#define CALC_RESULT_H    22
#define CALC_BTN_H       24
#define CALC_GAP          4

static void calc_layout(struct window *w, int *bx, int *by, int *bw,
                        int *ry, int *by1, int *eq_y, int *bw1) {
    *bx = CALC_PAD;
    *by = CALC_PAD;
    *bw = w->w - 2 * CALC_PAD;
    *ry = *by + CALC_FIELD_H + CALC_GAP;
    *by1 = *ry + CALC_RESULT_H + CALC_GAP;
    *bw1 = (*bw - 3) / 4;
    *eq_y = *by1 + 4 * (CALC_BTN_H + 1);
}

static void calc_draw(struct window *w, int frame) {
    calc_lazy_init();

    int bx, by, bw, ry, by1, eq_y, bw1;
    calc_layout(w, &bx, &by, &bw, &ry, &by1, &eq_y, &bw1);

    int X = w->x, Y = w->y + TITLE_H;

    tf_draw(&g_calc_tf, X + bx, Y + by, bw, CALC_FIELD_H,
            w->focused, frame);

    /* Result line — light grey, displays "= N" or "ERROR" or empty. */
    fill_rect(X + bx, Y + ry, bw, CALC_RESULT_H, 0xF0F0F0u);
    rect_outline(X + bx, Y + ry, bw, CALC_RESULT_H, 0x808080u);
    int ty = Y + ry + (CALC_RESULT_H - FONT_H) / 2;
    if (g_calc_err) {
        draw_str(X + bx + 6, ty, "ERROR", 0xC03030u);
    } else if (g_calc_have_result) {
        draw_str(X + bx + 6, ty, "= ", 0x202020u);
        draw_int(X + bx + 6 + 2 * FONT_W, ty, g_calc_result, 0x202020u);
    }

    /* 4x4 button grid. */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int x = X + bx + c * (bw1 + 1);
            int y = Y + by1 + r * (CALC_BTN_H + 1);
            uint32_t bgc = (g_calc_btn[r][c] == 'C') ? 0xE0A030u : 0xD0D0D0u;
            fill_rect(x, y, bw1, CALC_BTN_H, bgc);
            rect_outline(x, y, bw1, CALC_BTN_H, 0x606060u);
            char s[2] = { g_calc_btn[r][c], 0 };
            draw_str(x + (bw1 - FONT_W) / 2,
                     y + (CALC_BTN_H - FONT_H) / 2,
                     s, 0x101010u);
        }
    }

    /* Equals bar — full-width, vivid blue. */
    fill_rect(X + bx, Y + eq_y, bw, CALC_BTN_H, 0x4080E0u);
    rect_outline(X + bx, Y + eq_y, bw, CALC_BTN_H, 0x305080u);
    draw_str(X + bx + (bw - FONT_W) / 2,
             Y + eq_y + (CALC_BTN_H - FONT_H) / 2,
             "=", 0xFFFFFFu);
}

/* (lx, ly) are window-local coords with origin at (w->x + 0,
 * w->y + TITLE_H), as the WM passes to click handlers. */
static void calc_click(struct window *w, int lx, int ly, int btns) {
    /* Only fire on the initial down-edge — the WM also forwards drag
     * clicks here, which we don't want for buttons. The down-edge
     * detection happens in handle_mouse; this app always gets the
     * "is currently held" view, so guard explicitly. */
    (void)btns;
    calc_lazy_init();

    int bx, by, bw, ry, by1, eq_y, bw1;
    calc_layout(w, &bx, &by, &bw, &ry, &by1, &eq_y, &bw1);
    (void)by; (void)ry;

    if (ly >= eq_y && ly < eq_y + CALC_BTN_H) {
        calc_evaluate();
        return;
    }
    if (ly < by1 || ly >= by1 + 4 * (CALC_BTN_H + 1)) return;
    int r = (ly - by1) / (CALC_BTN_H + 1);
    int c = (lx - bx) / (bw1 + 1);
    if (r < 0 || r >= 4 || c < 0 || c >= 4) return;

    char ch = g_calc_btn[r][c];
    if (ch == 'C') {
        tf_clear(&g_calc_tf);
        g_calc_have_result = 0;
        g_calc_err = 0;
    } else {
        tf_append(&g_calc_tf, ch);
        g_calc_err = 0;
        g_calc_have_result = 0;
    }
}

static void calc_key(struct window *w, int key) {
    (void)w;
    calc_lazy_init();
    /* '=' as a keyboard shortcut for "evaluate". Real calculators
     * often treat = and Enter the same way. */
    if (key == '=' || key == '\r' || key == '\n') {
        calc_evaluate();
        return;
    }
    int submitted = tf_handle_key(&g_calc_tf, key);
    if (submitted) calc_evaluate();
    else {
        /* Editing — drop any stale result so the user isn't misled. */
        g_calc_have_result = 0;
        g_calc_err = 0;
    }
}

/* ---- Notepad app (session 61) -----------------------------------
 *
 * Multi-line text buffer with a "Save" button at the bottom that
 * writes the current contents to /notepad.txt. The text-field widget
 * does the editing — for notepad we re-purpose it as a "scratchpad
 * string" since the editing primitives (append, backspace, ENTER as
 * a regular character) are identical to a single-line field, just
 * with no enforced length limit and \n meaning "newline".
 *
 * Rendering is line-wrapped: walk the buffer, on \n or wrap-column
 * advance the cursor row. */

static char  g_notepad_buf[1024];
static struct text_field g_notepad_tf;
static int   g_notepad_inited;
static int   g_notepad_saved_msg_frames;   /* show "SAVED" for N frames */

static void notepad_lazy_init(void) {
    if (g_notepad_inited) return;
    tf_init(&g_notepad_tf, g_notepad_buf, (int)sizeof(g_notepad_buf));
    g_notepad_inited = 1;
}

static void notepad_save(void) {
    /* Persist the current buffer to /notepad.txt. The selftest reads
     * it back to verify the round-trip. */
    int rc = sys_fs_write("/notepad.txt",
                          g_notepad_tf.buf, (uint32_t)g_notepad_tf.len);
    printf("notepad: saved %d bytes to /notepad.txt (rc=%d)\n",
           g_notepad_tf.len, rc);
    g_notepad_saved_msg_frames = 60;        /* ~1s at 60 fps */
}

static void notepad_draw(struct window *w, int frame) {
    notepad_lazy_init();

    int pad = 8;
    int bx = w->x + pad;
    int by = w->y + TITLE_H + pad;
    int bw = w->w - 2 * pad;
    int bh = w->h - TITLE_H - 2 * pad - 28;  /* leave a button strip */

    /* Text-area background. */
    fill_rect(bx, by, bw, bh, 0xFFFFFFu);
    rect_outline(bx, by, bw, bh, w->focused ? 0x4080E0u : 0x808080u);
    if (w->focused) rect_outline(bx + 1, by + 1, bw - 2, bh - 2, 0x4080E0u);

    /* Word-wrap and newline-aware rendering. */
    int char_cap = (bw - 8) / FONT_W;
    if (char_cap < 1) char_cap = 1;
    int max_rows = (bh - 4) / FONT_H;
    if (max_rows < 1) max_rows = 1;
    int row = 0, col = 0;
    int last_x = bx + 4, last_y = by + 2;
    for (int i = 0; i < g_notepad_tf.len && row < max_rows; i++) {
        char c = g_notepad_tf.buf[i];
        if (c == '\n') {
            row++; col = 0;
            last_x = bx + 4;
            last_y = by + 2 + row * FONT_H;
            continue;
        }
        if (col >= char_cap) {
            row++; col = 0;
            last_x = bx + 4;
            last_y = by + 2 + row * FONT_H;
            if (row >= max_rows) break;
        }
        int gx = bx + 4 + col * FONT_W;
        int gy = by + 2 + row * FONT_H;
        draw_glyph(gx, gy, c, 0x101010u);
        last_x = gx + FONT_W;
        last_y = gy;
        col++;
    }
    /* Blinking cursor at the insertion point. */
    int blink_on = (frame / 15) & 1;
    if (w->focused && blink_on && row < max_rows) {
        draw_glyph(last_x, last_y, '_', 0x101010u);
    }

    /* Footer row: "Save" button + char count + transient SAVED label. */
    int fy = w->y + w->h - 28;
    fill_rect(bx, fy + 2, 60, 22, 0x40C060u);
    rect_outline(bx, fy + 2, 60, 22, 0x205040u);
    draw_str(bx + (60 - 4 * FONT_W) / 2,
             fy + 2 + (22 - FONT_H) / 2,
             "Save", 0xFFFFFFu);

    char counter[32];
    int ci = 0;
    const char *pfx = "  ";
    for (int j = 0; pfx[j]; j++) counter[ci++] = pfx[j];
    counter[ci++] = '(';
    int v = g_notepad_tf.len;
    if (v == 0) counter[ci++] = '0';
    else {
        char tmp[8]; int tn = 0;
        while (v) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        while (tn--) counter[ci++] = tmp[tn];
    }
    counter[ci++] = ' '; counter[ci++] = 'c'; counter[ci++] = 'h'; counter[ci++] = ')';
    counter[ci] = 0;
    draw_str(bx + 70, fy + 2 + (22 - FONT_H) / 2,
             counter, 0xC0C0C0u);

    if (g_notepad_saved_msg_frames > 0) {
        draw_str(bx + bw - 6 * FONT_W,
                 fy + 2 + (22 - FONT_H) / 2,
                 "SAVED!", 0x40C060u);
        g_notepad_saved_msg_frames--;
    }
}

static void notepad_click(struct window *w, int lx, int ly, int btns) {
    (void)btns;
    int pad = 8;
    /* The draw path computes the footer in SCREEN coords as
     *   fy_screen = w->y + (w->h - 28)
     * but click handlers receive body-local coords with origin at
     * (w->x, w->y + TITLE_H), so the same row in body-local is
     *   ly = (w->h - 28) - TITLE_H = w->h - 46
     * The Save button is +2px below that and 22px tall, matching
     * the fill_rect in notepad_draw. */
    int fy = w->h - 28 - TITLE_H;
    if (lx >= pad && lx < pad + 60 &&
        ly >= fy + 2 && ly < fy + 24) {
        notepad_save();
        return;
    }
}

static void notepad_key(struct window *w, int key) {
    (void)w;
    notepad_lazy_init();
    /* In notepad ENTER is just another character — append a literal
     * '\n' and keep going.  Ctrl-S would be nicer but we don't have
     * a modifier-key path through sys_kbd_poll. */
    if (key == '\r' || key == '\n') {
        tf_append(&g_notepad_tf, '\n');
        return;
    }
    tf_handle_key(&g_notepad_tf, key);
}

/* ---- Event dispatch + main loop --------------------------------- */

static void desktop_draw(int mx, int my, int btn, int frame) {
    (void)mx; (void)my; (void)btn;

    /* Desktop: solid steel-blue background. */
    fill_rect(0, 0, (int)g_w, (int)g_h, 0x103060u);

    /* Top "menu bar" — a 24-px strip. */
    fill_rect(0, 0, (int)g_w, 24, 0x282828u);
    draw_str(8,  8, "AdventOS WM  -  session 57", 0xC0C0C0u);
    draw_str((int)g_w - 100, 8, "frame:", 0x808080u);
    draw_int((int)g_w - 60,  8, frame,    0xC0C0C0u);

    /* Find z-sorted indices; tiny enough for n^2 sort. */
    int order[MAX_WINDOWS], n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].alive) order[n++] = i;
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (g_wins[order[j]].z < g_wins[order[i]].z) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
    for (int i = 0; i < n; i++) {
        struct window *w = &g_wins[order[i]];
        draw_window_frame(w);
        if (w->draw) w->draw(w, frame);
    }
}

static void handle_mouse(int mx, int my, int btns, int prev_btns) {
    /* Begin drag / focus on left-button DOWN edge. */
    int down_edge = (btns & 0x1) && !(prev_btns & 0x1);
    int up_edge   = !(btns & 0x1) && (prev_btns & 0x1);

    if (down_edge) {
        int idx = win_hit_test(mx, my);
        if (idx < 0) {
            for (int i = 0; i < MAX_WINDOWS; i++) g_wins[i].focused = 0;
            return;
        }
        win_raise(idx);
        struct window *w = &g_wins[idx];
        if (hit_close_button(w, mx, my)) {
            w->alive = 0;
            return;
        }
        if (hit_title_bar(w, mx, my)) {
            g_drag_idx = idx;
            g_drag_dx  = mx - w->x;
            g_drag_dy  = my - w->y;
            return;
        }
        /* Body click — local coords relative to content origin
         * (BELOW title bar). */
        int lx = mx - w->x;
        int ly = my - (w->y + TITLE_H);
        if (w->click) w->click(w, lx, ly, btns);
    } else if ((btns & 0x1) && g_drag_idx >= 0) {
        struct window *w = &g_wins[g_drag_idx];
        if (w->alive) {
            w->x = mx - g_drag_dx;
            w->y = my - g_drag_dy;
            /* Clamp to screen so a window can't escape interaction. */
            if (w->x < 0) w->x = 0;
            if (w->y < 24) w->y = 24;        /* below menu bar */
            if (w->x + w->w > (int)g_w) w->x = (int)g_w - w->w;
            if (w->y + w->h > (int)g_h) w->y = (int)g_h - w->h;
        }
    } else if ((btns & 0x1) && g_drag_idx < 0) {
        /* Continuous drag-paint on focused window — Paint opts into
         * this; button-style apps (Calc, Notepad) don't, so a held
         * click on a Save button only fires once on the down-edge. */
        int idx = win_focused_idx();
        if (idx >= 0) {
            struct window *w = &g_wins[idx];
            if (w->click && w->wants_drag && my > w->y + TITLE_H) {
                int lx = mx - w->x;
                int ly = my - (w->y + TITLE_H);
                w->click(w, lx, ly, btns);
            }
        }
    }
    if (up_edge) {
        g_drag_idx = -1;
    }
}

static void handle_key(int key) {
    int idx = win_focused_idx();
    if (idx < 0) return;
    struct window *w = &g_wins[idx];
    if (w->key) w->key(w, key);
}

/* ---- App registration ------------------------------------------- */

static void cp_title(struct window *w, const char *s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(w->title) - 1) {
        w->title[i] = s[i]; i++;
    }
    w->title[i] = 0;
}

static int spawn_window(const char *title, int x, int y, int width, int height,
                        draw_fn d, click_fn c, key_fn k) {
    int idx = win_alloc();
    if (idx < 0) return -1;
    struct window *w = &g_wins[idx];
    cp_title(w, title);
    w->x = x; w->y = y; w->w = width; w->h = height;
    w->draw  = d;
    w->click = c;
    w->key   = k;
    w->z     = ++g_next_z;
    w->focused = 1;
    return idx;
}

/* ---- Main loop --------------------------------------------------- */

static int g_selftest_mode = 0;

/* For the selftest we want determinism, not interactivity. The script
 * is a tiny ladder of (frame_at, x, y, btns) tuples that the WM
 * substitutes for real PS/2 events before reading the mouse state.
 * Each row is "at this frame, force the cursor to (x,y) with btns".
 * Between rows the cursor + buttons hold steady. */
struct script_step {
    int at_frame;
    int x, y, btns;
    /* Optional keystroke string: at this frame, inject these bytes
     * into the kernel keyboard ring so sys_kbd_poll returns them on
     * the next iteration of the WM event loop. NULL = no injection.
     * Session 61 added this to drive the text-field widget without
     * needing a physical keyboard. */
    const char *keys;
};

/* For t39 + t44: a single scripted timeline that drives both the
 * mouse-only Paint demo AND the new text-input apps (Calc, Notepad).
 *
 *   Frames 10-65:  Paint click + Hello title-bar drag (t39 coverage,
 *                  unchanged from session 57).
 *   Frames 80-150: Calc click + type "12+34" + ENTER → expect "= 46"
 *                  in stdout, plus the on-screen result.
 *   Frames 160-230: Notepad click + type "hi from gui" + click Save →
 *                   /notepad.txt holds the typed bytes, verified by
 *                   the t44 selftest after the WM exits.
 *
 * Pixel-exact assertions in t39 still grep these coords:
 *   Paint at  (300, 200)  size 360x280, body offset=(8,26)
 *     canvas top-left = (308, 226), cell size = 4
 *     cell (3, 1)    = pixels (320..323, 230..233)
 *   Hello at  (60, 60)    size 320x120, title bar y=61..77
 *     focused title color = 0x305080
 */
static struct script_step g_script[] = {
    /* Frame  X    Y    Btns Keys                  — note */
    {   5,   500, 400,   0,  0  },                   /* park */
    {  20,   322, 230,   1,  0  },                   /* Paint cell click */
    {  30,   322, 230,   1,  0  },                   /* hold */
    {  40,   322, 230,   0,  0  },                   /* release */
    {  50,   100, 70,    1,  0  },                   /* Hello drag */
    {  60,   100, 70,    0,  0  },                   /* release */

    /* ---- Calculator coverage (window @ 60, 200 size 240x300) ---- */
    {  80,   100, 230,   1,  0  },                   /* click into Calc text field */
    {  85,   100, 230,   0,  0  },                   /* release → window now focused */
    {  95,   100, 230,   0,  "12+34" },              /* type the expression */
    { 105,   100, 230,   0,  "\n" },                 /* ENTER = evaluate */

    /* ---- Notepad coverage (window @ 310, 490 size 360x180) ---- */
    { 130,   400, 540,   1,  0  },                   /* click into Notepad */
    { 135,   400, 540,   0,  0  },                   /* release */
    { 145,   400, 540,   0,  "hi from gui!" },       /* type a line */
    { 165,   324, 658,   1,  0  },                   /* click Save button */
    { 175,   324, 658,   0,  0  },                   /* release */

    /* Re-raise Hello so t39's "hello title bar = focused-blue"
     * pixel assertion holds at end-of-script. Without this, Notepad
     * would still be focused and Hello's title bar would read as
     * unfocused grey. */
    { 185,   100,  70,   1,  0  },
    { 195,   100,  70,   0,  0  },

    /* Sentinel — at_frame < 0 ends the table. */
    {  -1,    0,    0,   0,  0  },
};

/* Track the highest script index we've already applied so each
 * step's keystrokes fire EXACTLY ONCE — the naive "find last step
 * with at_frame <= frame" approach would re-inject every frame past
 * the trigger, dumping the same string into the keyboard ring sixty
 * times a second. */
static int g_last_script_idx = -1;

static void script_apply(int frame, int *mx, int *my, int *btns) {
    int last = -1;
    for (int i = 0; g_script[i].at_frame >= 0; i++) {
        if (g_script[i].at_frame <= frame) last = i;
    }
    if (last < 0) return;

    /* Mouse position holds at the latest applied step's value — that
     * matches "the user's hand is wherever it was." */
    *mx   = g_script[last].x;
    *my   = g_script[last].y;
    *btns = g_script[last].btns;
    sys_mouse_inject(*mx, *my, *btns);

    /* Keystrokes are one-shot: inject each new row's bytes the first
     * frame it becomes the active step. */
    if (last != g_last_script_idx) {
        for (int i = g_last_script_idx + 1; i <= last; i++) {
            const char *keys = g_script[i].keys;
            if (keys) {
                int kn = 0; while (keys[kn]) kn++;
                if (kn > 0) tty_inject(keys, kn);
            }
        }
        g_last_script_idx = last;
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && argv[1][0] == 's') g_selftest_mode = 1;

    unsigned int info[4] = {0};
    if (!sys_fbinfo(info)) {
        puts("gui: no framebuffer — exiting cleanly\n");
        return 0;
    }
    g_w = info[0]; g_h = info[1]; g_bpp = info[2]; g_pitch = info[3];
    g_fb = sys_fb_mmap();
    if (!g_fb) { puts("gui: SYS_FB_MMAP failed\n"); return 1; }

    /* Stop kernel fbcon writes so our WM owns every pixel for the
     * duration of the run. */
    sys_fb_takeover(1);

    printf("gui-wm: %ux%u @ %u-bpp, fb at 0x%x  selftest=%d\n",
           g_w, g_h, g_bpp, (unsigned)(uintptr_t)g_fb, g_selftest_mode);

    /* Pick a starting palette for the Paint app. */
    g_paint_canvas[0] = 0;
    g_wins[0].state[0] = 0xE03030;   /* red */

    spawn_window("Hello",   60,  60, 320, 120,
                 hello_draw, 0, 0);
    spawn_window("Clock",  420,  60, 260, 140,
                 clock_draw, 0, 0);
    spawn_window("Calc",    60, 200, 240, 270,
                 calc_draw, calc_click, calc_key);
    int paint_idx = spawn_window("Paint",  310, 200, 360, 280,
                 paint_draw, paint_click, 0);
    g_wins[paint_idx].wants_drag = 1;        /* drag-to-paint */
    spawn_window("Notepad",310, 490, 360, 180,
                 notepad_draw, notepad_click, notepad_key);
    spawn_window("Tasks",  680, 220, 280, 240,
                 tasks_draw, 0, 0);

    int frame = 0;
    int prev_btns = 0;
    /* Selftest now runs longer (was 80 frames in session 57) — the
     * Calc + Notepad coverage scripted in g_script[] extends to
     * frame ~175, and we want a few settling frames past the last
     * Save click to redraw the "SAVED" footer overlay. */
    int max_frames = g_selftest_mode ? 200 : 600;

    for (;;) {
        int ms[4] = {0,0,0,0};
        int mx, my, btns;

        if (g_selftest_mode) {
            sys_mouse_state(ms);
            mx = ms[0]; my = ms[1]; btns = ms[2];
            script_apply(frame, &mx, &my, &btns);
        } else {
            sys_mouse_state(ms);
            mx = ms[0]; my = ms[1]; btns = ms[2];
        }

        /* Keystroke (non-blocking). ESC quits. */
        int key = sys_kbd_poll();
        if (key) {
            if (key == 0x1B) break;
            handle_key(key);
        }

        handle_mouse(mx, my, btns, prev_btns);
        prev_btns = btns;

        desktop_draw(mx, my, btns, frame);
        draw_cursor(mx, my);

        /* Status overlay — bottom-right corner. */
        int sy = (int)g_h - FONT_H - 4;
        draw_str((int)g_w - 200, sy, "x:", 0xFFFFFFu);
        draw_int((int)g_w - 184, sy, mx,    0xFFFFFFu);
        draw_str((int)g_w - 150, sy, "y:", 0xFFFFFFu);
        draw_int((int)g_w - 134, sy, my,    0xFFFFFFu);
        draw_str((int)g_w - 100, sy, "btn:", 0xFFFFFFu);
        draw_int((int)g_w - 70,  sy, btns,  0xFFFFFFu);

        sys_sleep_ms(16);
        frame++;
        if (frame >= max_frames) break;
    }

    /* Release the framebuffer + restore fbcon before exit. */
    sys_fb_takeover(0);

    if (g_selftest_mode) {
        /* Print one summary line + a pixel snapshot at known coords.
         * t39's grep keys off these.  See the script comment above
         * for how these coords line up with Paint's canvas cell (3,1)
         * and Hello's title bar after the final click+raise. */
        printf("gui-wm: selftest done, %d frames\n", frame);
        printf("  paint cell @ (322,232) = 0x%x\n",
               get_pixel(322, 232));
        printf("  hello title bar @ (110,70) = 0x%x\n",
               get_pixel(110, 70));
        printf("  desktop bg @ (0,30) = 0x%x\n",
               get_pixel(0, 30));
    } else {
        puts("gui-wm: exit\n");
    }
    return 0;
}
