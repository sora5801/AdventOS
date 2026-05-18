/*
 * wmd.c — session 111 / Path C phase 5 window manager daemon.
 *
 *   wmd [seconds]      grab the framebuffer, composite a handful
 *                      of internal demo windows, drive a mouse
 *                      cursor on top.  Run for SECONDS, default 30.
 *
 * No client protocol yet — that lands in session 112. This daemon
 * paints a fixed set of windows out of its own pixel buffers so we
 * can demonstrate compositing + window decorations + mouse-driven
 * z-order changes + click-and-drag on title bars, all without yet
 * needing IPC for client surface sharing.
 *
 * Each tick:
 *   1. poll mouse, handle press/release/drag
 *   2. clear backbuffer (root window background)
 *   3. for each window bottom-to-top: paint decoration + content
 *   4. paint cursor on top
 *   5. gfx_present
 *
 * Compositing model is back-to-front blit; no per-window damage
 * tracking yet (every tick = full repaint of every window). Cheap
 * because the windows are small and the backbuffer is RAM.
 *
 * Path C status after this: we now have multiple "windows" on the
 * same screen at the same time. Next: real client surfaces (112).
 */

#include "libuser.h"
#include "../libgfx/libgfx.h"

#define FB_VA      0x50000000u
#define MAX_WINDOWS 6
#define TITLE_H    18
#define CURSOR_R   8

struct window {
    int  x, y;          /* top-left of decoration */
    int  w, h;          /* full size including title bar */
    char title[28];
    unsigned int frame_color;
    unsigned int content_color;
    int  kind;          /* 0=clock, 1=gradient, 2=text, 3=color-bars */
    int  raised;        /* z-order: higher = on top */
};

static struct window g_windows[MAX_WINDOWS];
static int g_window_count;
static int g_z_counter = 1;

/* Drag state. window_idx == -1 means no drag. */
static int g_drag_idx = -1;
static int g_drag_off_x, g_drag_off_y;
static int g_prev_left;

static int my_atoi_str(const char *s) { return atoi(s); }

/* Format an unsigned int into buf[]; return chars written. Used by
 * the clock window and gradient labels (libuser snprintf is absent). */
static int fmt_u(char *buf, int cap, unsigned int v) {
    char t[12]; int n = 0;
    if (v == 0) { t[n++] = '0'; }
    else { while (v && n < 12) { t[n++] = '0' + (v % 10); v /= 10; } }
    int out = 0;
    while (n-- > 0 && out < cap - 1) buf[out++] = t[n];
    buf[out] = 0;
    return out;
}

static void draw_cursor(struct gfx_ctx *ctx, int cx, int cy, unsigned int rgb) {
    gfx_line(ctx, cx - CURSOR_R, cy, cx + CURSOR_R, cy, rgb);
    gfx_line(ctx, cx, cy - CURSOR_R, cx, cy + CURSOR_R, rgb);
}

/* Window content painters. */
static void paint_clock(struct gfx_ctx *ctx, struct window *w,
                        unsigned int t_sec, unsigned int frame_no) {
    int x = w->x + 2;
    int y = w->y + TITLE_H + 4;
    gfx_fill_rect(ctx, x, y, w->w - 4, w->h - TITLE_H - 6, w->content_color);
    /* sys_time is seconds only — display whole-second uptime, plus
     * a frame counter that ticks every 16 ms so we can see the
     * compositor is actually running between SYS_TIME ticks. */
    char buf[40]; int n = 0;
    const char *p = "uptime ";
    while (*p && n < (int)sizeof(buf) - 1) buf[n++] = *p++;
    n += fmt_u(buf + n, (int)sizeof(buf) - n, t_sec);
    if (n < (int)sizeof(buf) - 1) buf[n++] = ' ';
    if (n < (int)sizeof(buf) - 1) buf[n++] = 's';
    buf[n] = 0;
    gfx_text(ctx, x + 6, y + 8, buf, GFX_WHITE, GFX_TRANSPARENT);
    char fbuf[40]; int fn = 0;
    p = "frame ";
    while (*p && fn < (int)sizeof(fbuf) - 1) fbuf[fn++] = *p++;
    fn += fmt_u(fbuf + fn, (int)sizeof(fbuf) - fn, frame_no);
    gfx_text(ctx, x + 6, y + 24, fbuf, GFX_GREY, GFX_TRANSPARENT);
}

static void paint_gradient(struct gfx_ctx *ctx, struct window *w) {
    int x = w->x + 2;
    int y = w->y + TITLE_H + 4;
    int gw = w->w - 4;
    int gh = w->h - TITLE_H - 6;
    /* Row-major gradient: R changes with x, G changes with y, B fixed. */
    for (int yy = 0; yy < gh; yy++) {
        unsigned int g = (unsigned int)yy * 255u / (gh > 1 ? gh - 1 : 1);
        for (int xx = 0; xx < gw; xx++) {
            unsigned int r = (unsigned int)xx * 255u / (gw > 1 ? gw - 1 : 1);
            unsigned int rgb = (r << 16) | (g << 8) | 0x80u;
            gfx_put_pixel(ctx, x + xx, y + yy, rgb);
        }
    }
    gfx_text(ctx, x + 6, y + 6, "RGB gradient",
             GFX_WHITE, GFX_TRANSPARENT);
}

static void paint_text(struct gfx_ctx *ctx, struct window *w) {
    int x = w->x + 2;
    int y = w->y + TITLE_H + 4;
    gfx_fill_rect(ctx, x, y, w->w - 4, w->h - TITLE_H - 6, w->content_color);
    gfx_text(ctx, x + 6, y +  6, "AdventOS wmd",  GFX_WHITE, GFX_TRANSPARENT);
    gfx_text(ctx, x + 6, y + 22, "session 111",   GFX_GREY,  GFX_TRANSPARENT);
    gfx_text(ctx, x + 6, y + 38, "drag titlebars", GFX_GREEN, GFX_TRANSPARENT);
    gfx_text(ctx, x + 6, y + 54, "click to raise", GFX_GREEN, GFX_TRANSPARENT);
}

static void paint_bars(struct gfx_ctx *ctx, struct window *w) {
    int x = w->x + 2;
    int y = w->y + TITLE_H + 4;
    int gw = w->w - 4;
    int gh = w->h - TITLE_H - 6;
    unsigned int bars[] = {
        GFX_RED, GFX_YELLOW, GFX_GREEN,
        GFX_CYAN, GFX_BLUE, GFX_MAGENTA
    };
    int n_bars = (int)(sizeof(bars) / sizeof(bars[0]));
    int bw = gw / n_bars;
    if (bw < 1) bw = 1;
    for (int i = 0; i < n_bars; i++) {
        gfx_fill_rect(ctx, x + i * bw, y, bw, gh, bars[i]);
    }
}

/* Paint a single window's frame + title bar + content. */
static void paint_window(struct gfx_ctx *ctx, struct window *w,
                         int has_focus, unsigned int t_sec,
                         unsigned int frame_no) {
    /* Outer drop-shadow (1 px black to the right and below). Tiny
     * touch but distinguishes overlapping windows even without
     * alpha blending. */
    gfx_fill_rect(ctx, w->x + 2, w->y + w->h, w->w, 2, GFX_BLACK);
    gfx_fill_rect(ctx, w->x + w->w, w->y + 2, 2, w->h, GFX_BLACK);

    /* Title bar. */
    unsigned int title_bg = has_focus ? w->frame_color : GFX_DARK_GREY;
    gfx_fill_rect(ctx, w->x, w->y, w->w, TITLE_H, title_bg);
    gfx_text(ctx, w->x + 6, w->y + 5, w->title,
             GFX_WHITE, GFX_TRANSPARENT);

    /* Frame outline. */
    gfx_rect(ctx, w->x, w->y, w->w, w->h, GFX_WHITE);
    gfx_line(ctx, w->x, w->y + TITLE_H - 1,
             w->x + w->w - 1, w->y + TITLE_H - 1, GFX_WHITE);

    /* Content. */
    gfx_fill_rect(ctx, w->x + 1, w->y + TITLE_H,
                  w->w - 2, w->h - TITLE_H - 1, w->content_color);
    switch (w->kind) {
        case 0: paint_clock(ctx, w, t_sec, frame_no); break;
        case 1: paint_gradient(ctx, w); break;
        case 2: paint_text(ctx, w); break;
        case 3: paint_bars(ctx, w); break;
        default: break;
    }
}

/* Insertion sort by raised value, ascending — paint order: bottom→top.
 * Stable so windows with equal raise stay in declared order. */
static void z_order(int *out) {
    for (int i = 0; i < g_window_count; i++) out[i] = i;
    for (int i = 1; i < g_window_count; i++) {
        int j = i;
        while (j > 0 && g_windows[out[j-1]].raised > g_windows[out[j]].raised) {
            int tmp = out[j-1]; out[j-1] = out[j]; out[j] = tmp;
            j--;
        }
    }
}

/* Returns -1 if no window contains (px, py); otherwise the window
 * index with the highest z. Searched top-to-bottom. */
static int hit_test(int px, int py) {
    int order[MAX_WINDOWS];
    z_order(order);
    /* Top→bottom in z. */
    for (int i = g_window_count - 1; i >= 0; i--) {
        struct window *w = &g_windows[order[i]];
        if (px >= w->x && px < w->x + w->w &&
            py >= w->y && py < w->y + w->h) {
            return order[i];
        }
    }
    return -1;
}

static int in_titlebar(struct window *w, int px, int py) {
    return px >= w->x && px < w->x + w->w &&
           py >= w->y && py < w->y + TITLE_H;
}

static void init_demo_windows(unsigned int fb_w, unsigned int fb_h) {
    (void)fb_w; (void)fb_h;
    g_window_count = 4;

    g_windows[0].x = 80;  g_windows[0].y = 80;
    g_windows[0].w = 260; g_windows[0].h = 110;
    g_windows[0].frame_color   = GFX_BLUE;
    g_windows[0].content_color = 0x102030u;
    g_windows[0].kind = 0;
    g_windows[0].raised = 1;
    {const char *t = "Clock"; int i=0; while(t[i] && i<27){g_windows[0].title[i]=t[i];i++;} g_windows[0].title[i]=0;}

    g_windows[1].x = 400; g_windows[1].y = 120;
    g_windows[1].w = 260; g_windows[1].h = 170;
    g_windows[1].frame_color   = GFX_MAGENTA;
    g_windows[1].content_color = GFX_BLACK;
    g_windows[1].kind = 1;
    g_windows[1].raised = 2;
    {const char *t = "Gradient"; int i=0; while(t[i] && i<27){g_windows[1].title[i]=t[i];i++;} g_windows[1].title[i]=0;}

    g_windows[2].x = 180; g_windows[2].y = 260;
    g_windows[2].w = 260; g_windows[2].h = 130;
    g_windows[2].frame_color   = GFX_GREEN;
    g_windows[2].content_color = 0x103018u;
    g_windows[2].kind = 2;
    g_windows[2].raised = 3;
    {const char *t = "About wmd"; int i=0; while(t[i] && i<27){g_windows[2].title[i]=t[i];i++;} g_windows[2].title[i]=0;}

    g_windows[3].x = 540; g_windows[3].y = 360;
    g_windows[3].w = 320; g_windows[3].h = 110;
    g_windows[3].frame_color   = GFX_YELLOW;
    g_windows[3].content_color = GFX_BLACK;
    g_windows[3].kind = 3;
    g_windows[3].raised = 4;
    {const char *t = "Color bars"; int i=0; while(t[i] && i<27){g_windows[3].title[i]=t[i];i++;} g_windows[3].title[i]=0;}
}

int main(int argc, char **argv) {
    int seconds = 30;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 30;

    struct gfx_ctx ctx;
    if (gfx_init_db(&ctx, FB_VA) < 0) {
        printf("wmd: framebuffer unavailable or already owned\n");
        return 1;
    }

    init_demo_windows(ctx.width, ctx.height);

    unsigned int t0 = sys_time();
    int total_ticks = seconds * 60;
    int focused = -1;
    g_prev_left = 0;

    for (int tick = 0; tick < total_ticks; tick++) {
        struct sys_mouse_state ms;
        if (sys_mouse_poll(&ms) < 0) break;
        unsigned int t_now = sys_time();
        unsigned int t_sec = (t_now >= t0) ? (t_now - t0) : 0u;

        int left = (ms.buttons & 0x01) ? 1 : 0;
        int pressed  = left && !g_prev_left;
        int released = !left && g_prev_left;

        if (pressed) {
            int hit = hit_test(ms.x, ms.y);
            if (hit >= 0) {
                /* Raise. */
                g_z_counter++;
                g_windows[hit].raised = g_z_counter;
                focused = hit;
                /* Start drag if in title bar. */
                if (in_titlebar(&g_windows[hit], ms.x, ms.y)) {
                    g_drag_idx   = hit;
                    g_drag_off_x = ms.x - g_windows[hit].x;
                    g_drag_off_y = ms.y - g_windows[hit].y;
                }
            } else {
                focused = -1;
            }
        }
        if (released) {
            g_drag_idx = -1;
        }
        if (g_drag_idx >= 0) {
            struct window *w = &g_windows[g_drag_idx];
            int nx = ms.x - g_drag_off_x;
            int ny = ms.y - g_drag_off_y;
            /* Clamp inside FB so the titlebar stays grabbable. */
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > (int)ctx.width  - w->w) nx = (int)ctx.width  - w->w;
            if (ny > (int)ctx.height - w->h) ny = (int)ctx.height - w->h;
            w->x = nx;
            w->y = ny;
        }
        g_prev_left = left;

        /* Compose the frame. */
        gfx_clear(&ctx, 0x0A1828u);  /* deep blue desktop bg */

        /* Top status bar across the screen. */
        gfx_fill_rect(&ctx, 0, 0, (int)ctx.width, 18, GFX_DARK_GREY);
        gfx_text(&ctx, 8, 5, "wmd - AdventOS Path C session 111",
                 GFX_WHITE, GFX_TRANSPARENT);

        int order[MAX_WINDOWS];
        z_order(order);
        for (int i = 0; i < g_window_count; i++) {
            int idx = order[i];
            paint_window(&ctx, &g_windows[idx],
                         idx == focused, t_sec, (unsigned int)tick);
        }

        /* Cursor: red if dragging, otherwise white. */
        unsigned int cursor_rgb = (g_drag_idx >= 0) ? GFX_RED : GFX_WHITE;
        if (left && g_drag_idx < 0) cursor_rgb = GFX_YELLOW; /* click on bg */
        draw_cursor(&ctx, ms.x, ms.y, cursor_rgb);

        gfx_present(&ctx);
        sys_sleep_ms(16);
    }

    gfx_release(&ctx);
    printf("wmd: released\n");
    return 0;
}
