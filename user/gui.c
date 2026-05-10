/*
 * gui — a tiny ring-3 graphics demo.
 *
 * mmaps the framebuffer via SYS_FB_MMAP, polls the mouse via
 * SYS_MOUSE_STATE, and draws:
 *   - a 3-color background gradient banner
 *   - a tile of "windows" with hit-testable click regions
 *   - a 12x12 mouse cursor that tracks the PS/2 mouse
 *   - a status bar showing (x, y, btns, fps)
 *
 * Exits when the user hits ESC on the keyboard. The kernel signals
 * the keyboard via SYS_READ_LINE — but that blocks. Since this is a
 * polling demo, we use a one-shot peek: SYS_TTY_INJECT-only path
 * isn't available, so we just run forever in the headless test
 * (kmain never types ESC). Real interactive use sees ESC echoed
 * via the serial port through the keyboard buffer.
 *
 * NOT forked: gui.elf is launched by init.elf as a one-shot. fork()
 * after sys_fb_mmap would deep-copy the FB pages via
 * paging_clone_user_pd, which would corrupt the user PD's view of
 * the FB (the clone allocates fresh pages and memcpy's the FB
 * contents into them). Avoid for now.
 */

#include "libuser.h"

/* Cached framebuffer geometry (filled by setup()). */
static volatile unsigned char *g_fb;
static unsigned int      g_w, g_h, g_bpp, g_pitch;

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

static void fill_rect(int x0, int y0, int w, int h, unsigned int rgb) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            put_pixel(x0 + x, y0 + y, rgb);
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

/* ---- Tiny 5x7 digit font for the status bar ----------------------
 * Each glyph is 7 rows tall, encoded as one byte per row with bit
 * 4 = leftmost pixel (5 columns). Just digits + a few punctuation
 * marks; enough for "x:1234 y:567 btn:1 fps:60". */
static const unsigned char gly5x7[16][7] = {
    /* '0' */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* '1' */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* '2' */ {0x0E,0x11,0x10,0x08,0x04,0x02,0x1F},
    /* '3' */ {0x1F,0x08,0x04,0x08,0x10,0x11,0x0E},
    /* '4' */ {0x08,0x0C,0x0A,0x09,0x1F,0x08,0x08},
    /* '5' */ {0x1F,0x01,0x0F,0x10,0x10,0x11,0x0E},
    /* '6' */ {0x0C,0x02,0x01,0x0F,0x11,0x11,0x0E},
    /* '7' */ {0x1F,0x10,0x08,0x04,0x02,0x02,0x02},
    /* '8' */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* '9' */ {0x0E,0x11,0x11,0x1E,0x10,0x08,0x06},
    /* ':' */ {0x00,0x06,0x06,0x00,0x06,0x06,0x00},
    /* ' ' */ {0,0,0,0,0,0,0},
    /* 'x' */ {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
    /* 'y' */ {0x00,0x00,0x11,0x11,0x1E,0x10,0x0E},
    /* 'b' */ {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F},
    /* 't' */ {0x02,0x02,0x07,0x02,0x02,0x12,0x0C},
};
/* Map char to glyph index, -1 if unsupported. */
static int gly_idx(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
        case ':': return 10;
        case ' ': return 11;
        case 'x': return 12;
        case 'y': return 13;
        case 'b': return 14;
        case 't': return 15;
        default:  return -1;
    }
}
static void draw_char(int x, int y, char c, unsigned int rgb) {
    int idx = gly_idx(c);
    if (idx < 0) return;
    for (int r = 0; r < 7; r++) {
        unsigned char bits = gly5x7[idx][r];
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col))) put_pixel(x + col, y + r, rgb);
        }
    }
}
static void draw_str(int x, int y, const char *s, unsigned int rgb) {
    for (; *s; s++) {
        draw_char(x, y, *s, rgb);
        x += 6;
    }
}
static void draw_int(int x, int y, int n, unsigned int rgb) {
    char buf[12]; int i = 0;
    if (n < 0) { n = -n; }
    if (n == 0) buf[i++] = '0';
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) { draw_char(x, y, buf[i], rgb); x += 6; }
}

/* ---- 12x12 cursor sprite ----------------------------------------
 * Classic arrow shape. Each row is 12 bits wide (LSB = leftmost).
 * Two layers: outline (drawn first, in BLACK) and fill (WHITE on
 * top). The outline gives 1px contrast on any background. */
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

/* ---- demo content -----------------------------------------------
 * Static "background" — gradient header bar, three "window" tiles
 * with their own backgrounds. Drawn ONCE at startup; per-frame the
 * GUI just overlays the cursor + status bar. We keep a "saved
 * pixels" copy of the previous cursor's bbox so each frame can
 * restore them before re-drawing the cursor at the new position. */

static int  g_ui_h;       /* height of the "fixed" UI area below header */
static unsigned char g_save[12 * 12 * 4];   /* up to 32-bpp per pixel */
static int  g_last_x = -1, g_last_y = -1;

static void draw_background(void) {
    /* Gradient header — top 60 px, sweeping cyan→blue. */
    for (int y = 0; y < 60; y++) {
        unsigned int t = y * 255 / 60;
        unsigned int r = 0x10;
        unsigned int g = 0x40 + (t >> 1);
        unsigned int b = 0xA0 + (t >> 2);
        unsigned int rgb = (r << 16) | (g << 8) | b;
        for (unsigned int x = 0; x < g_w; x++) put_pixel((int)x, y, rgb);
    }
    /* Title text — coarse "AdventOS" via 5x7 chars. We don't have
     * letters in our glyph set so just draw "00:00" marker for the
     * sake of having something; the title is a strip of color tile
     * marks below. */
    for (int i = 0; i < 8; i++) {
        unsigned int hue = (i & 1) ? 0xFFFFFF : 0x88CCFF;
        fill_rect(20 + i * 20, 26, 14, 8, hue);
    }

    /* Body — solid grey background. */
    fill_rect(0, 60, (int)g_w, (int)g_h - 60, 0x1A1A1A);

    /* Three click-target tiles. */
    int tile_w = 200, tile_h = 140, gap = 30, top = 100;
    int total_w = tile_w * 3 + gap * 2;
    int x0 = ((int)g_w - total_w) / 2;
    unsigned int colors[3] = { 0x4080E0u, 0xE0A030u, 0x30C040u };
    const char *labels[3]  = { "tile 0", "tile 1", "tile 2" };
    for (int i = 0; i < 3; i++) {
        int x = x0 + i * (tile_w + gap);
        fill_rect(x, top, tile_w, tile_h, colors[i]);
        rect_outline(x, top, tile_w, tile_h, 0xFFFFFF);
        /* Label: just the index; our font doesn't have letters
         * besides x/y/b/t so ignore the literal text and draw a
         * big number instead. */
        for (int s = 0; s < 4; s++) {
            draw_int(x + 12 + s, top + 12 + s, i, 0xFFFFFF);
        }
        (void)labels;
    }
    g_ui_h = top + tile_h + 30;

    /* Status bar background — bottom 20 px. */
    fill_rect(0, (int)g_h - 20, (int)g_w, 20, 0x000000);
}

/* Save/restore the 12x12 pixel block under the cursor so the
 * background isn't permanently smeared. */
static void cursor_save(int x, int y) {
    int bpp = (g_bpp + 7) / 8;
    for (int r = 0; r < 12; r++) {
        for (int c = 0; c < 12; c++) {
            int xx = x + c, yy = y + r;
            if (xx < 0 || yy < 0 || (unsigned)xx >= g_w || (unsigned)yy >= g_h) continue;
            volatile unsigned char *p = g_fb + (unsigned)yy * g_pitch + xx * bpp;
            unsigned char *dst = &g_save[(r * 12 + c) * 4];
            for (int k = 0; k < bpp; k++) dst[k] = p[k];
        }
    }
}
static void cursor_restore(int x, int y) {
    int bpp = (g_bpp + 7) / 8;
    for (int r = 0; r < 12; r++) {
        for (int c = 0; c < 12; c++) {
            int xx = x + c, yy = y + r;
            if (xx < 0 || yy < 0 || (unsigned)xx >= g_w || (unsigned)yy >= g_h) continue;
            volatile unsigned char *p = g_fb + (unsigned)yy * g_pitch + xx * bpp;
            unsigned char *src = &g_save[(r * 12 + c) * 4];
            for (int k = 0; k < bpp; k++) p[k] = src[k];
        }
    }
}

/* Status-bar repaint. Always redrawn at full each frame so old
 * values don't ghost. */
static void draw_status(int mx, int my, int btns, int frame) {
    int sy = (int)g_h - 14;
    fill_rect(0, (int)g_h - 20, (int)g_w, 20, 0x000000);
    int x = 8;
    draw_str(x, sy, "x:",   0xFFFFFF); x += 12;
    draw_int(x, sy, mx,     0xFFFF60); x += 30;
    draw_str(x, sy, "y:",   0xFFFFFF); x += 12;
    draw_int(x, sy, my,     0xFFFF60); x += 30;
    draw_str(x, sy, "b:",   0xFFFFFF); x += 12;
    draw_int(x, sy, btns,   (btns ? 0x60FF60 : 0x808080)); x += 30;
    draw_str(x, sy, "t:",   0xFFFFFF); x += 12;
    draw_int(x, sy, frame,  0xFFFF60);
}

/* Tile click: highlight tile briefly when cursor is inside and
 * left button held. */
static void maybe_highlight(int mx, int my, int btns) {
    int tile_w = 200, tile_h = 140, gap = 30, top = 100;
    int total_w = tile_w * 3 + gap * 2;
    int x0 = ((int)g_w - total_w) / 2;
    if (!(btns & 1)) return;
    for (int i = 0; i < 3; i++) {
        int tx = x0 + i * (tile_w + gap);
        if (mx >= tx && mx < tx + tile_w &&
            my >= top && my < top + tile_h) {
            /* Inset 2-px white frame inside the tile. */
            for (int k = 2; k < 8; k++) rect_outline(tx + k, top + k,
                                                    tile_w - 2*k,
                                                    tile_h - 2*k,
                                                    0xFFFFFF);
        }
    }
}

int main(void) {
    /* Headless safety: if VBE didn't come up, exit cleanly. */
    unsigned int info[4] = {0};
    int fbon = sys_fbinfo(info);
    if (!fbon) {
        puts("gui: no framebuffer available — exiting\n");
        return 0;
    }
    g_w     = info[0];
    g_h     = info[1];
    g_bpp   = info[2];
    g_pitch = info[3];

    g_fb = sys_fb_mmap();
    if (!g_fb) {
        puts("gui: SYS_FB_MMAP failed — exiting\n");
        return 0;
    }
    printf("gui: %ux%u @ %u-bpp, FB mapped at 0x%x\n",
           g_w, g_h, g_bpp, (unsigned)(unsigned long)g_fb);

    draw_background();

    int frame = 0;
    while (1) {
        int ms[4] = {0,0,0,0};
        sys_mouse_state(ms);
        int mx = ms[0], my = ms[1], btns = ms[2];

        /* Restore old cursor area so background isn't smeared. */
        if (g_last_x >= 0) cursor_restore(g_last_x, g_last_y);

        /* Click-highlight overlay — drawn UNDER the cursor. */
        maybe_highlight(mx, my, btns);

        /* Save the new area, then paint cursor on top. */
        cursor_save(mx, my);
        draw_cursor(mx, my);

        draw_status(mx, my, btns, frame);

        g_last_x = mx;
        g_last_y = my;
        frame++;

        sys_sleep_ms(16);   /* ~60 fps target */

        /* Headless / selftest exit: cap at 240 frames (~4 sec) so
         * the test harness can verify+screenshot+continue. The
         * shell selftest spawns gui.elf and reaps it. */
        if (frame > 240) break;
    }
    puts("gui: 240 frames done, exiting\n");
    return 0;
}
