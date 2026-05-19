/*
 * wmpaint.c — session 115 WM client.
 *
 *   wmpaint [seconds]    open a 400x280 paint canvas (default 60s).
 *
 * Drag with left button to draw; release to lift the pen.  Press a
 * digit key 1..7 to change the brush color.  Press 'c' to clear.
 * Press 'q' to quit early.
 *
 * Demonstrates a stateful client: mouse press → enter drawing mode,
 * mouse move while pressed → paint, release → exit drawing mode.
 * The canvas is the surface itself — pixels persist across frames
 * because we don't clear, only overlay the toolbar each tick.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W 400
#define WIN_H 280
#define TOOLBAR_H 24

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

/* Brush palette indexed 1..7 by the digit key. */
static const unsigned int g_brushes[8] = {
    0xFFFFFFu, 0xFFFFFFu,        /* 0 unused, 1 = white */
    0xE03030u,                   /* 2 = red */
    0xE0E030u,                   /* 3 = yellow */
    0x30E030u,                   /* 4 = green */
    0x30E0E0u,                   /* 5 = cyan */
    0x4080E0u,                   /* 6 = blue */
    0xE030E0u,                   /* 7 = magenta */
};

/* Bresenham line into the surface, for smooth strokes between
 * MOUSE_MOVE events when the cursor moves more than 1 pixel. */
static void draw_line(struct wm_window *w, int x0, int y0, int x1, int y1,
                      unsigned int rgb, int radius) {
    int dx =  (x1 > x0 ? x1 - x0 : x0 - x1);
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int oy = -radius; oy <= radius; oy++) {
            for (int ox = -radius; ox <= radius; ox++) {
                if (ox * ox + oy * oy > radius * radius) continue;
                wm_put_pixel(w, x0 + ox, y0 + oy, rgb);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Session 150 — write the canvas (everything below the toolbar)
 * to /tmp/paint.ppm as a binary P6 PPM.  Streams one row at a
 * time so we don't have to materialise a 300 KB temp buffer in
 * the wmpaint .bin blob.  wm_notify confirms the save. */
static void itoa_app(char *buf, int *pos, int v) {
    char t[12]; int n = 0;
    if (v == 0) { t[n++] = '0'; }
    else { while (v) { t[n++] = '0' + (v % 10); v /= 10; } }
    while (n-- > 0) buf[(*pos)++] = t[n];
}

static void save_canvas(struct wm_window *win) {
    int fd = sys_open_w("/tmp/paint.ppm");
    if (fd < 0) {
        wm_notify("wmpaint: save failed");
        return;
    }
    int canvas_w = WIN_W;
    int canvas_h = WIN_H - TOOLBAR_H;

    /* Header. */
    char hdr[32]; int hn = 0;
    hdr[hn++] = 'P'; hdr[hn++] = '6'; hdr[hn++] = '\n';
    itoa_app(hdr, &hn, canvas_w);
    hdr[hn++] = ' ';
    itoa_app(hdr, &hn, canvas_h);
    hdr[hn++] = '\n';
    hdr[hn++] = '2'; hdr[hn++] = '5'; hdr[hn++] = '5'; hdr[hn++] = '\n';
    sys_write(fd, hdr, hn);

    /* One row buffer = 1200 bytes; on the stack is fine. */
    unsigned int *pix   = win->pixels;
    unsigned int  pitch = win->pitch_px;
    unsigned char row[WIN_W * 3];
    int total_bytes = hn;
    for (int y = TOOLBAR_H; y < WIN_H; y++) {
        for (int x = 0; x < WIN_W; x++) {
            unsigned int c = pix[y * pitch + x];
            row[x*3 + 0] = (unsigned char)((c >> 16) & 0xFF);
            row[x*3 + 1] = (unsigned char)((c >>  8) & 0xFF);
            row[x*3 + 2] = (unsigned char)( c        & 0xFF);
        }
        sys_write(fd, row, canvas_w * 3);
        total_bytes += canvas_w * 3;
    }
    sys_close(fd);

    /* Build the toast: "saved /tmp/paint.ppm (NNN B)" */
    char tn[64]; int tnn = 0;
    const char *m = "saved /tmp/paint.ppm (";
    while (*m && tnn < (int)sizeof(tn) - 1) tn[tnn++] = *m++;
    itoa_app(tn, &tnn, total_bytes);
    if (tnn < (int)sizeof(tn) - 3) {
        tn[tnn++] = ' '; tn[tnn++] = 'B'; tn[tnn++] = ')';
    }
    tn[tnn] = 0;
    wm_notify(tn);
}

/* Redraw the top toolbar — gets called every frame because the
 * paint canvas is the surface itself and we don't full-clear. */
static void paint_toolbar(struct wm_window *win, struct gfx_ctx *sctx,
                          int brush_idx, int radius, int has_focus) {
    wm_fill_rect(win, 0, 0, WIN_W, TOOLBAR_H,
                 has_focus ? 0x202830u : 0x202020u);

    /* Color swatches 1..7. */
    for (int i = 1; i <= 7; i++) {
        int x = 4 + (i - 1) * 22;
        wm_fill_rect(win, x, 4, 16, 16, g_brushes[i]);
        if (i == brush_idx) {
            /* highlight the selected swatch with a white border */
            wm_fill_rect(win, x - 1, 3, 18, 1, 0xFFFFFFu);
            wm_fill_rect(win, x - 1, 20, 18, 1, 0xFFFFFFu);
            wm_fill_rect(win, x - 1, 3, 1, 18, 0xFFFFFFu);
            wm_fill_rect(win, x + 17, 3, 1, 18, 0xFFFFFFu);
        }
    }

    /* Hint text. */
    gfx_text(sctx, 4 + 7 * 22 + 8, 8,
             "1..7 color  c clear  Ctrl-S save  q quit",
             GFX_GREY, GFX_TRANSPARENT);
    (void)radius;
}

int main(int argc, char **argv) {
    int seconds = 60;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 60;

    struct wm_window win;
    if (wm_open(&win, "wmpaint", WIN_W, WIN_H) < 0) {
        printf("wmpaint: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmpaint: id=%u\n", win.id);

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    /* Fill the canvas area dark grey, leave it filled forever. */
    wm_clear(&win, 0x282828u);

    int brush_idx = 1;            /* white */
    int radius    = 2;
    int drawing   = 0;
    int last_x = -1, last_y = -1;
    int has_focus = 0;
    int total_ticks = seconds * 30;
    int quit = 0;

    for (int tick = 0; tick < total_ticks && !quit; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_MOUSE_PRESS:
                    /* Toolbar click selects color. */
                    if (ev.y < TOOLBAR_H && ev.y >= 4) {
                        int sx = (ev.x - 4) / 22;
                        if (sx >= 0 && sx < 7) {
                            brush_idx = sx + 1;
                            break;
                        }
                    }
                    /* Otherwise begin a stroke. */
                    drawing = 1;
                    last_x = ev.x;
                    last_y = ev.y;
                    /* Plant a dot immediately so a tap shows. */
                    draw_line(&win, ev.x, ev.y, ev.x, ev.y,
                              g_brushes[brush_idx], radius);
                    break;
                case WM_EV_MOUSE_RELEASE:
                    drawing = 0;
                    last_x = last_y = -1;
                    break;
                case WM_EV_MOUSE_MOVE:
                    if (drawing && ev.y >= TOOLBAR_H) {
                        int x = ev.x, y = ev.y;
                        if (last_x < 0) {
                            last_x = x; last_y = y;
                        }
                        draw_line(&win, last_x, last_y, x, y,
                                  g_brushes[brush_idx], radius);
                        last_x = x; last_y = y;
                    }
                    break;
                case WM_EV_KEY: {
                    unsigned int k = ev.keycode;
                    if (k >= '1' && k <= '7') brush_idx = (int)(k - '0');
                    else if (k == 'c' || k == 'C') {
                        wm_fill_rect(&win, 0, TOOLBAR_H,
                                     WIN_W, WIN_H - TOOLBAR_H, 0x282828u);
                    }
                    else if (k == 'q' || k == 'Q') quit = 1;
                    else if (k == 0x13) {         /* Ctrl-S save */
                        save_canvas(&win);
                    }
                    else if (k == '+' || k == '=') {
                        if (radius < 6) radius++;
                    } else if (k == '-' || k == '_') {
                        if (radius > 0) radius--;
                    }
                    break;
                }
                case WM_EV_CLOSE:
                    quit = 1;
                    break;
                default: break;
            }
        }

        paint_toolbar(&win, &sctx, brush_idx, radius, has_focus);
        wm_present(&win);
        sys_sleep_ms(33);
    }

    wm_close(&win);
    printf("wmpaint: done\n");
    return 0;
}
