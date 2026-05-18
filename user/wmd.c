/*
 * wmd.c — sessions 111 + 112 window manager daemon.
 *
 *   wmd [seconds]      grab the framebuffer, composite a handful
 *                      of internal demo windows AND any client
 *                      surfaces registered via SYS_WM_CREATE.
 *                      Run for SECONDS, default 30.
 *
 * Session 111 introduced compositing of in-process "demo" windows
 * (clock / gradient / text / bars).  Session 112 adds the WM client
 * protocol: external programs allocate shared pixel surfaces via
 * sys_wm_create; wmd polls sys_wm_poll each tick to discover new
 * surfaces and blit them into the compositor's frame.
 *
 * Each tick:
 *   1. drain sys_wm_poll → add / remove client-backed windows
 *   2. poll mouse, handle press/release/drag
 *   3. clear backbuffer (root window background)
 *   4. for each window bottom-to-top: paint decoration + content
 *      (content for kind=CLIENT is a per-pixel blit from the shared
 *      32-bpp surface)
 *   5. paint cursor on top
 *   6. gfx_present
 *
 * Compositing model is back-to-front blit; no per-window damage
 * tracking yet (every tick = full repaint of every window). Cheap
 * because the windows are small and the backbuffer is RAM.
 */

#include "libuser.h"
#include "../libgfx/libgfx.h"

#define FB_VA      0x50000000u
#define MAX_WINDOWS 8
#define TITLE_H    18
#define CURSOR_R   8
/* Session 118 — bottom taskbar height.  Reserved real-estate at
 * y = ctx.height - TASKBAR_H .. ctx.height.  Buttons are uniform
 * width within that strip. */
#define TASKBAR_H  28
#define TASKBAR_BTN_W  140
#define TASKBAR_BTN_PAD  4
/* Session 119 — Start button on the left edge of the taskbar
 * plus the launcher popup that opens above it. */
#define START_BTN_W  64
#define LAUNCH_ITEM_H 22
#define LAUNCH_W    160

#define KIND_CLOCK   0
#define KIND_GRADIENT 1
#define KIND_TEXT    2
#define KIND_BARS    3
#define KIND_CLIENT  4

struct window {
    int  x, y;          /* top-left of decoration */
    int  w, h;          /* full size including title bar */
    char title[28];
    unsigned int frame_color;
    unsigned int content_color;
    int  kind;
    int  raised;        /* z-order: higher = on top */

    /* For KIND_CLIENT only: pointer to the shared 32-bpp surface
     * mapped by the kernel into our PD at wm_create_window time.
     * Pitch is `surface_w * 4` (packed). */
    unsigned int  client_id;
    unsigned int *client_pixels;
    unsigned int  surface_w, surface_h;

    /* Session 133 — min/max state.  When `minimized` is set, the
     * compositor skips painting this window entirely (taskbar
     * button still shown so the user can click it to unhide).
     * When `maximized` is set, x/y/w/h have been overwritten to
     * fill the entire usable FB and the saved_* fields hold the
     * previous values so a second click on the max button can
     * restore. */
    int  minimized;
    int  maximized;
    int  saved_x, saved_y, saved_w, saved_h;
};

static struct window g_windows[MAX_WINDOWS];
static int g_window_count;
static int g_z_counter = 1;

/* Drag state. window_idx == -1 means no drag. */
/* Session 119 — app launcher catalog.  Keep paths in lockstep with
 * mkfs.py's `files` table. */
struct launch_entry {
    const char *label;
    const char *path;
};
static const struct launch_entry g_launch_items[] = {
    { "wmhello", "/wmhello.elf" },
    { "wmtype",  "/wmtype.elf"  },
    { "wmclock", "/wmclock.elf" },
    { "wmpaint", "/wmpaint.elf" },
    { "wmpair",  "/wmpair.elf"  },
    { "wmfiles", "/wmfiles.elf" },
    { "sysinfo", "/wmsysinfo.elf" },
    { "wmps",    "/wmps.elf"     },
    { "wmterm",  "/wmterm.elf"   },
};
#define N_LAUNCH_ITEMS  ((int)(sizeof(g_launch_items) / sizeof(g_launch_items[0])))
static int g_launcher_open;

/* Session 124 — right-click window context menu.  Open when a
 * RIGHT-click lands on a client window's title bar; closes on the
 * next click anywhere.  Targets a window by client_id so it
 * survives slot-array compaction. */
struct ctx_menu_state {
    int          open;
    int          x, y;          /* top-left of popup */
    unsigned int target_id;
};
static struct ctx_menu_state g_ctx_menu;

#define CTXMENU_W       100
#define CTXMENU_ITEM_H  18
#define CTXMENU_N_ITEMS 2

static const char *g_ctx_labels[CTXMENU_N_ITEMS] = {
    "Raise",
    "Close",
};

static int g_drag_idx = -1;
static int g_drag_off_x, g_drag_off_y;
static int g_prev_left;
static int g_prev_right;       /* session 124 — right-button edge */

/* Session 131 — window resize.  When the user drags the 12x12 grip
 * in the bottom-right corner of a CLIENT window, we set
 * g_resize_idx to that window's slot and update its outer (w, h)
 * on every motion event until the mouse releases.  The client
 * surface dimensions stay at the original surface_w / surface_h —
 * paint_client clips to min(window w, surface w) so a smaller
 * window just shows less of the surface, and a larger window pads
 * with the content_color background fill paint_window already
 * does.  This means resize stays a pure WM concern; no client
 * cooperation needed. */
#define RESIZE_GRIP 12
#define WIN_MIN_W   80
#define WIN_MIN_H   60
static int g_resize_idx = -1;
static int g_resize_anchor_w, g_resize_anchor_h;
static int g_resize_anchor_mx, g_resize_anchor_my;

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

/* Session 112 — paint a client-backed window's interior by per-pixel
 * blitting the shared 32-bpp surface into the backbuffer. The
 * surface dimensions are surface_w x surface_h; if they don't match
 * the window's content area (w-2, h-TITLE_H-2), the difference is
 * left as the content-color background fill that paint_window
 * already wrote. */
static void paint_client(struct gfx_ctx *ctx, struct window *w) {
    if (!w->client_pixels) return;
    int dest_x = w->x + 1;
    int dest_y = w->y + TITLE_H;
    int max_w = w->w - 2;
    int max_h = w->h - TITLE_H - 1;
    int blit_w = (int)w->surface_w < max_w ? (int)w->surface_w : max_w;
    int blit_h = (int)w->surface_h < max_h ? (int)w->surface_h : max_h;
    for (int yy = 0; yy < blit_h; yy++) {
        const unsigned int *row = w->client_pixels
                                + (unsigned int)yy * w->surface_w;
        for (int xx = 0; xx < blit_w; xx++) {
            gfx_put_pixel(ctx, dest_x + xx, dest_y + yy, row[xx]);
        }
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

    /* Sessions 116 + 133 — title-bar buttons on every CLIENT
     * window.  Layout from right to left: [close] [maximize]
     * [minimize].  Each is a 14×14 square with a single-character
     * glyph; the colours encode the action: red close, green
     * maximize, yellow minimize.  Click handling lives in the
     * main loop; here we just paint. */
    if (w->kind == KIND_CLIENT) {
        int by = w->y + 2;
        int cbx = w->x + w->w - 16;
        int mxbx = cbx - 16;
        int mnbx = mxbx - 16;
        gfx_fill_rect(ctx, mnbx, by, 14, 14, GFX_YELLOW);
        gfx_text(ctx, mnbx + 3, by + 3, "_", GFX_WHITE, GFX_TRANSPARENT);
        gfx_fill_rect(ctx, mxbx, by, 14, 14, GFX_GREEN);
        gfx_text(ctx, mxbx + 3, by + 3,
                 w->maximized ? "o" : "[", GFX_WHITE, GFX_TRANSPARENT);
        gfx_fill_rect(ctx, cbx, by, 14, 14, GFX_RED);
        gfx_text(ctx, cbx + 3, by + 3, "x", GFX_WHITE, GFX_TRANSPARENT);
    }

    /* Frame outline. */
    gfx_rect(ctx, w->x, w->y, w->w, w->h, GFX_WHITE);
    gfx_line(ctx, w->x, w->y + TITLE_H - 1,
             w->x + w->w - 1, w->y + TITLE_H - 1, GFX_WHITE);

    /* Content. */
    gfx_fill_rect(ctx, w->x + 1, w->y + TITLE_H,
                  w->w - 2, w->h - TITLE_H - 1, w->content_color);
    switch (w->kind) {
        case KIND_CLOCK:    paint_clock(ctx, w, t_sec, frame_no); break;
        case KIND_GRADIENT: paint_gradient(ctx, w); break;
        case KIND_TEXT:     paint_text(ctx, w); break;
        case KIND_BARS:     paint_bars(ctx, w); break;
        case KIND_CLIENT:   paint_client(ctx, w); break;
        default: break;
    }

    /* Session 131 — bottom-right resize grip.  Painted LAST so it
     * sits on top of the content fill and any client-surface blit;
     * otherwise the content background fill (or paint_client) would
     * overwrite the grip pixels.  Three diagonal pixel-stripes
     * inside a 12×12 box.  CLIENT only. */
    if (w->kind == KIND_CLIENT) {
        int gx = w->x + w->w - RESIZE_GRIP;
        int gy = w->y + w->h - RESIZE_GRIP;
        gfx_fill_rect(ctx, gx, gy, RESIZE_GRIP, RESIZE_GRIP,
                      w->frame_color);
        for (int i = 2; i < RESIZE_GRIP - 1; i += 3) {
            for (int k = 0; k <= i; k++) {
                gfx_put_pixel(ctx,
                              gx + RESIZE_GRIP - 1 - k,
                              gy + RESIZE_GRIP - 1 - (i - k),
                              GFX_WHITE);
            }
        }
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
        /* Session 133 — minimized windows don't accept input. */
        if (w->minimized) continue;
        if (px >= w->x && px < w->x + w->w &&
            py >= w->y && py < w->y + w->h) {
            return order[i];
        }
    }
    return -1;
}

/* Session 118 — taskbar.  Iterate the CLIENT windows in
 * registration order (i.e. the order they appear in g_windows[]),
 * give each one a fixed-width button.  Session 119 — the first
 * START_BTN_W pixels are reserved for the Start button.  Session
 * 121 — the last 132 px are reserved for the system clock; stop
 * iterating before we collide with it. */
static int taskbar_hit(int fb_w, int fb_h, int px, int py) {
    if (py < fb_h - TASKBAR_H || py >= fb_h) return -1;
    int x = START_BTN_W + TASKBAR_BTN_PAD;
    int right_limit = fb_w - 132 - TASKBAR_BTN_PAD;
    for (int i = 0; i < g_window_count; i++) {
        if (g_windows[i].kind != KIND_CLIENT) continue;
        if (x + TASKBAR_BTN_W > right_limit) break;
        int x2 = x + TASKBAR_BTN_W;
        if (px >= x && px < x2) return i;
        x = x2 + TASKBAR_BTN_PAD;
    }
    return -1;
}

/* Session 119 — was the click in the Start button at the very
 * left of the taskbar? */
static int start_button_hit(int fb_w, int fb_h, int px, int py) {
    (void)fb_w;
    if (py < fb_h - TASKBAR_H + 4 || py >= fb_h - 4) return 0;
    if (px < 4 || px >= START_BTN_W - 4) return 0;
    return 1;
}

/* Session 119 — when the launcher is open, was the click on one
 * of its items?  Returns the item index (0..N) or -1. */
static int launcher_hit(int fb_h, int px, int py) {
    if (!g_launcher_open) return -1;
    int ly = fb_h - TASKBAR_H - N_LAUNCH_ITEMS * LAUNCH_ITEM_H - 4;
    int lx = 4;
    if (px < lx || px >= lx + LAUNCH_W) return -1;
    if (py < ly || py >= ly + N_LAUNCH_ITEMS * LAUNCH_ITEM_H) return -1;
    return (py - ly) / LAUNCH_ITEM_H;
}

/* Session 127 — procedural desktop wallpaper.  Renders a subtle
 * "starfield-ish" diagonal-gradient pattern across the FB before
 * any window paints.  Replaces the flat 0x0A1828 fill that wmd had
 * shipped with since session 111.  Stays cheap (a single pass of
 * gfx_fill_rect bands plus a sparse seeded-pseudo-random dot
 * pattern) so it doesn't impact the 60-fps frame budget.
 *
 * The pattern is deterministic — no rng state — so the smoke
 * tests get a stable bg to sample. */
static void paint_wallpaper(struct gfx_ctx *ctx) {
    int fb_w = (int)ctx->width;
    int fb_h = (int)ctx->height;

    /* Vertical gradient — 8 bands of subtly different blue,
     * centred on the legacy 0x0A1828 colour so existing smoke
     * tests that sample the desktop bg with a small tolerance
     * still see colours that pass. */
    int bands = 8;
    int band_h = fb_h / bands;
    if (band_h <= 0) band_h = 1;
    for (int i = 0; i < bands; i++) {
        int t = i - bands / 2;            /* -4..+3 */
        int r = 0x0A + t / 2;
        int g = 0x18 + t;
        int b = 0x28 + t;
        unsigned int color = ((unsigned)r << 16) | ((unsigned)g << 8)
                           | (unsigned)b;
        int y = i * band_h;
        int h = (i == bands - 1) ? (fb_h - y) : band_h;
        gfx_fill_rect(ctx, 0, y, fb_w, h, color);
    }

    /* Subtle "stars" — a deterministic sparse dot grid that
     * traces a moiré-like diagonal pattern.  Approximately 1 star
     * per 256 px, weighted toward the centre by a simple xor
     * mask. */
    for (int y = 24; y < fb_h - 32; y += 16) {
        for (int x = 12; x < fb_w; x += 16) {
            unsigned int h = ((unsigned int)x * 73u
                            + (unsigned int)y * 197u) & 0xFFu;
            if (h < 32) {
                unsigned int v = 0x40 + h;
                unsigned int color = (v << 16) | (v << 8) | (v + 32);
                gfx_put_pixel(ctx, x, y, color);
            }
        }
    }
}

static void paint_taskbar(struct gfx_ctx *ctx, int focused_idx) {
    int fb_w = (int)ctx->width;
    int fb_h = (int)ctx->height;
    int y    = fb_h - TASKBAR_H;
    /* Bar background. */
    gfx_fill_rect(ctx, 0, y, fb_w, TASKBAR_H, 0x182030u);
    gfx_line(ctx, 0, y, fb_w - 1, y, GFX_GREY);

    /* Session 119 — Start button on the very left. */
    int sby = y + 4;
    int sbh = TASKBAR_H - 8;
    unsigned int sfill = g_launcher_open ? GFX_GREEN : 0x205030u;
    gfx_fill_rect(ctx, 4, sby, START_BTN_W - 8, sbh, sfill);
    gfx_rect(ctx, 4, sby, START_BTN_W - 8, sbh, GFX_WHITE);
    gfx_text(ctx, 4 + 6, sby + 5, "Start",
             GFX_WHITE, GFX_TRANSPARENT);

    /* Session 121 — clock on the right side of the taskbar.
     * 2x font for HH:MM (10 chars × 16 px = 160 px wide).  We
     * reserve 132 px on the right so the leftmost char starts at
     * x = fb_w - 132 + 4.  Per-window buttons stop short of this
     * reservation. */
    int clock_w = 132;
    int clock_x = fb_w - clock_w;
    {
        unsigned int ts = sys_time();
        unsigned int min = (ts / 60u) % 60u;
        unsigned int hr  = (ts / 3600u) % 24u;
        char buf[6];
        buf[0] = '0' + (char)((hr / 10) % 10);
        buf[1] = '0' + (char)(hr % 10);
        buf[2] = ':';
        buf[3] = '0' + (char)(min / 10);
        buf[4] = '0' + (char)(min % 10);
        buf[5] = 0;
        /* Centre vertically: bar is TASKBAR_H tall (28), 2x font is
         * 16 px → top offset = (28 - 16) / 2 = 6. */
        gfx_text_n(ctx, clock_x + 4, y + 6, buf, 2,
                   GFX_WHITE, GFX_TRANSPARENT);
    }

    /* Per-window button.  Starts after the Start button.  Stops
     * before the clock reservation so labels never collide with
     * digits. */
    int bx = START_BTN_W + TASKBAR_BTN_PAD;
    int by = y + 4;
    int bh = TASKBAR_H - 8;
    int btn_right_limit = clock_x - TASKBAR_BTN_PAD;
    for (int i = 0; i < g_window_count; i++) {
        struct window *w = &g_windows[i];
        if (w->kind != KIND_CLIENT) continue;
        if (bx + TASKBAR_BTN_W > btn_right_limit) break;
        int is_focused = (i == focused_idx);
        unsigned int fill = is_focused ? w->frame_color : 0x303848u;
        gfx_fill_rect(ctx, bx, by, TASKBAR_BTN_W, bh, fill);
        gfx_rect(ctx, bx, by, TASKBAR_BTN_W, bh,
                 is_focused ? GFX_WHITE : GFX_GREY);
        /* Truncate title to fit (~16 chars at 8 px each). */
        gfx_text(ctx, bx + 6, by + 5, w->title,
                 GFX_WHITE, GFX_TRANSPARENT);
        bx += TASKBAR_BTN_W + TASKBAR_BTN_PAD;
    }
}

/* Session 124 — right-click context menu painter + hit-test. */
static void paint_ctx_menu(struct gfx_ctx *ctx) {
    if (!g_ctx_menu.open) return;
    int x = g_ctx_menu.x;
    int y = g_ctx_menu.y;
    int h = CTXMENU_N_ITEMS * CTXMENU_ITEM_H;
    /* Shadow + body. */
    gfx_fill_rect(ctx, x + 2, y + h, CTXMENU_W, 2, GFX_BLACK);
    gfx_fill_rect(ctx, x + CTXMENU_W, y + 2, 2, h, GFX_BLACK);
    gfx_fill_rect(ctx, x, y, CTXMENU_W, h, 0x202830u);
    gfx_rect    (ctx, x, y, CTXMENU_W, h, GFX_WHITE);
    for (int i = 0; i < CTXMENU_N_ITEMS; i++) {
        int iy = y + i * CTXMENU_ITEM_H;
        if (i > 0) gfx_line(ctx, x + 1, iy, x + CTXMENU_W - 2, iy,
                            0x404850u);
        gfx_text(ctx, x + 8, iy + 5, g_ctx_labels[i],
                 GFX_WHITE, GFX_TRANSPARENT);
    }
}

static int ctx_menu_hit(int px, int py) {
    if (!g_ctx_menu.open) return -1;
    int x = g_ctx_menu.x;
    int y = g_ctx_menu.y;
    int h = CTXMENU_N_ITEMS * CTXMENU_ITEM_H;
    if (px < x || px >= x + CTXMENU_W) return -1;
    if (py < y || py >= y + h) return -1;
    return (py - y) / CTXMENU_ITEM_H;
}

/* Session 119 — popup over the Start button when the launcher is
 * open.  Drawn AFTER paint_taskbar so it sits on top. */
static void paint_launcher(struct gfx_ctx *ctx) {
    if (!g_launcher_open) return;
    int fb_h = (int)ctx->height;
    int ly = fb_h - TASKBAR_H - N_LAUNCH_ITEMS * LAUNCH_ITEM_H - 4;
    int lx = 4;
    int lh = N_LAUNCH_ITEMS * LAUNCH_ITEM_H;
    /* Drop shadow. */
    gfx_fill_rect(ctx, lx + 2, ly + lh, LAUNCH_W, 2, GFX_BLACK);
    gfx_fill_rect(ctx, lx + LAUNCH_W, ly + 2, 2, lh, GFX_BLACK);
    /* Body. */
    gfx_fill_rect(ctx, lx, ly, LAUNCH_W, lh, 0x202830u);
    gfx_rect    (ctx, lx, ly, LAUNCH_W, lh, GFX_WHITE);
    for (int i = 0; i < N_LAUNCH_ITEMS; i++) {
        int iy = ly + i * LAUNCH_ITEM_H;
        if (i > 0) gfx_line(ctx, lx + 1, iy, lx + LAUNCH_W - 2, iy,
                            0x404850u);
        gfx_text(ctx, lx + 10, iy + 7, g_launch_items[i].label,
                 GFX_WHITE, GFX_TRANSPARENT);
    }
}

static int in_titlebar(struct window *w, int px, int py) {
    return px >= w->x && px < w->x + w->w &&
           py >= w->y && py < w->y + TITLE_H;
}

/* Session 131 — is (px,py) inside the bottom-right resize grip?
 * Only CLIENT windows get a grip; demo windows ignore resize. */
static int in_resize_grip(struct window *w, int px, int py) {
    if (w->kind != KIND_CLIENT) return 0;
    int gx = w->x + w->w - RESIZE_GRIP;
    int gy = w->y + w->h - RESIZE_GRIP;
    return px >= gx && px < gx + RESIZE_GRIP &&
           py >= gy && py < gy + RESIZE_GRIP;
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

    /* Subsequent click-to-raise / WM-CREATE bumps need to go ABOVE
     * the four demo windows, so bring g_z_counter up to the highest
     * raised value we just assigned. */
    g_z_counter = 4;
}

/* Session 112 — find a g_windows[] entry by its client id (assigned
 * by the kernel and returned in sys_wm_msg).  Returns NULL if no
 * such slot. */
static struct window *find_window_by_client_id(unsigned int id) {
    for (int i = 0; i < g_window_count; i++) {
        if (g_windows[i].kind == KIND_CLIENT &&
            g_windows[i].client_id == id) return &g_windows[i];
    }
    return 0;
}

/* Drain any pending WM messages; returns the number processed.
 * Called once per tick. */
static int drain_wm_messages(unsigned int fb_w, unsigned int fb_h) {
    int n = 0;
    for (;;) {
        struct sys_wm_msg m;
        int r = sys_wm_poll(&m);
        if (r <= 0) break;
        n++;
        if (m.op == 1) {
            /* Open. Place the new window at a deterministic offset
             * so multiple test clients don't all overlap. */
            if (g_window_count >= MAX_WINDOWS) continue;
            struct window *w = &g_windows[g_window_count++];
            int total_w = (int)m.w + 4;          /* +2px border each side */
            int total_h = (int)m.h + TITLE_H + 2;
            int slot = g_window_count - 1;
            w->x = 100 + slot * 60;
            w->y = 200 + slot * 40;
            if (w->x + total_w > (int)fb_w - 8) w->x = (int)fb_w - 8 - total_w;
            if (w->y + total_h > (int)fb_h - 8) w->y = (int)fb_h - 8 - total_h;
            w->w = total_w;
            w->h = total_h;
            for (int i = 0; i < 27 && m.title[i]; i++) w->title[i] = m.title[i];
            w->title[27] = 0;
            w->frame_color   = GFX_CYAN;
            w->content_color = GFX_BLACK;
            w->kind          = KIND_CLIENT;
            g_z_counter++;
            w->raised = g_z_counter;
            w->client_id     = m.id;
            w->client_pixels = (unsigned int *)(uintptr_t)m.wmd_va;
            w->surface_w     = m.w;
            w->surface_h     = m.h;
        } else if (m.op == 2) {
            /* Destroy. Compact the array. */
            struct window *w = find_window_by_client_id(m.id);
            if (!w) continue;
            int idx = (int)(w - g_windows);
            for (int i = idx; i < g_window_count - 1; i++)
                g_windows[i] = g_windows[i + 1];
            g_window_count--;
        }
    }
    return n;
}

int main(int argc, char **argv) {
    int seconds = 30;
    int show_demos = 1;
    /* Session 123 — argv parsing: positional SECONDS plus an
     * optional --clean flag to suppress the four daemon-internal
     * demonstration windows from session 111.  Demos default ON
     * for backward compatibility with the smoke-test suite that
     * verifies their presence; --clean gives end users a tidy
     * desktop without them. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') {
            if (a[2] == 'c') show_demos = 0;
        } else {
            int v = my_atoi_str(a);
            if (v > 0) seconds = v;
        }
    }

    struct gfx_ctx ctx;
    if (gfx_init_db(&ctx, FB_VA) < 0) {
        printf("wmd: framebuffer unavailable or already owned\n");
        return 1;
    }

    /* Session 112 — claim the WM role so client SYS_WM_CREATE calls
     * find a compositor.  If another wmd is already running, exit
     * gracefully (the FB grab above would have already failed in
     * that case but be explicit). */
    if (sys_wm_bind() < 0) {
        printf("wmd: another window manager is already bound\n");
        gfx_release(&ctx);
        return 1;
    }

    if (show_demos) init_demo_windows(ctx.width, ctx.height);

    unsigned int t0 = sys_time();
    int total_ticks = seconds * 60;
    int focused = -1;
    g_prev_left = 0;

    /* Session 113 — track which client window the cursor was over
     * last tick so we can synthesize HOVER_ENTER/LEAVE edges when
     * it crosses a window boundary.  Session 117 — separate from
     * `prev_focus_id` (kbd focus, changes on click). */
    int prev_hover_idx = -1;
    int prev_focus_id  = 0;       /* 0 = nothing, else client_id */
    int prev_mx = -1, prev_my = -1;

    for (int tick = 0; tick < total_ticks; tick++) {
        /* Session 112 — drain WM client events first so newly-
         * registered windows appear on this frame. */
        drain_wm_messages(ctx.width, ctx.height);

        struct sys_mouse_state ms;
        if (sys_mouse_poll(&ms) < 0) break;
        unsigned int t_now = sys_time();
        unsigned int t_sec = (t_now >= t0) ? (t_now - t0) : 0u;

        int left = (ms.buttons & 0x01) ? 1 : 0;
        int pressed  = left && !g_prev_left;
        int released = !left && g_prev_left;

        /* Session 124 — right-button edge for the context menu. */
        int right = (ms.buttons & 0x02) ? 1 : 0;
        int right_pressed = right && !g_prev_right;
        g_prev_right = right;

        if (right_pressed) {
            /* If the menu is already open, close it. */
            if (g_ctx_menu.open) {
                g_ctx_menu.open = 0;
            } else {
                int hit = hit_test(ms.x, ms.y);
                if (hit >= 0 && g_windows[hit].kind == KIND_CLIENT
                    && in_titlebar(&g_windows[hit], ms.x, ms.y)) {
                    g_ctx_menu.open      = 1;
                    g_ctx_menu.target_id = g_windows[hit].client_id;
                    g_ctx_menu.x         = ms.x;
                    g_ctx_menu.y         = ms.y;
                    if (g_ctx_menu.x + CTXMENU_W > (int)ctx.width)
                        g_ctx_menu.x = (int)ctx.width - CTXMENU_W;
                    if (g_ctx_menu.y + CTXMENU_N_ITEMS * CTXMENU_ITEM_H
                        > (int)ctx.height)
                        g_ctx_menu.y = (int)ctx.height
                            - CTXMENU_N_ITEMS * CTXMENU_ITEM_H;
                }
            }
        }

        if (pressed) {
            /* Session 124 — context-menu item click intercept. */
            if (g_ctx_menu.open) {
                int item = ctx_menu_hit(ms.x, ms.y);
                /* Find target window by client_id. */
                int target_idx = -1;
                for (int i = 0; i < g_window_count; i++) {
                    if (g_windows[i].kind == KIND_CLIENT
                        && g_windows[i].client_id == g_ctx_menu.target_id) {
                        target_idx = i; break;
                    }
                }
                if (item == 0 && target_idx >= 0) {
                    /* Raise. */
                    g_z_counter++;
                    g_windows[target_idx].raised = g_z_counter;
                    focused = target_idx;
                } else if (item == 1 && target_idx >= 0) {
                    /* Close. */
                    struct sys_wm_event ev = {0};
                    ev.type = WM_EV_CLOSE;
                    sys_wm_event_push(g_ctx_menu.target_id, &ev);
                }
                g_ctx_menu.open = 0;
                goto after_press_hit;
            }
            /* Session 119 — launcher popup intercept.  If the popup
             * is open and the click landed on an item, fork+exec
             * the chosen program.  If the click is anywhere else,
             * close the popup. */
            if (g_launcher_open) {
                int li = launcher_hit((int)ctx.height, ms.x, ms.y);
                if (li >= 0) {
                    int pid = sys_fork();
                    if (pid == 0) {
                        const char *argv[2] = { g_launch_items[li].path, 0 };
                        sys_exec(g_launch_items[li].path, argv);
                        sys_exit(127);   /* exec failed */
                    }
                }
                g_launcher_open = 0;
                goto after_press_hit;
            }
            if (start_button_hit((int)ctx.width, (int)ctx.height,
                                 ms.x, ms.y)) {
                g_launcher_open = 1;
                goto after_press_hit;
            }
            /* Session 118 — taskbar buttons live below all
             * windows in z order.  Check them BEFORE the normal
             * hit_test so a button click always raises the right
             * window even if it's behind another one. */
            int tb_hit = taskbar_hit((int)ctx.width, (int)ctx.height,
                                     ms.x, ms.y);
            if (tb_hit >= 0) {
                /* Session 133 — clicking a taskbar button restores
                 * a minimized window in addition to raising +
                 * focusing it. */
                g_windows[tb_hit].minimized = 0;
                g_z_counter++;
                g_windows[tb_hit].raised = g_z_counter;
                focused = tb_hit;
                /* Don't trigger raise via hit_test below. */
                goto after_press_hit;
            }
            int hit = hit_test(ms.x, ms.y);
            if (hit >= 0) {
                /* Sessions 116 + 133 — title-bar button intercepts.
                 * Three 14x14 boxes from right to left at the top
                 * of every CLIENT window: close (red), maximize
                 * (green), minimize (yellow).  Each consumes the
                 * click entirely (no raise, no drag, no focus
                 * change beyond what the button itself triggers). */
                struct window *cw = &g_windows[hit];
                int button_hit = 0;
                if (cw->kind == KIND_CLIENT) {
                    int by = cw->y + 2;
                    int cbx = cw->x + cw->w - 16;
                    int mxbx = cbx - 16;
                    int mnbx = mxbx - 16;
                    if (ms.y >= by && ms.y < by + 14) {
                        if (ms.x >= cbx && ms.x < cbx + 14) {
                            /* close */
                            button_hit = 1;
                            struct sys_wm_event ev = {0};
                            ev.type = WM_EV_CLOSE;
                            sys_wm_event_push(cw->client_id, &ev);
                        } else if (ms.x >= mxbx && ms.x < mxbx + 14) {
                            /* maximize toggle */
                            button_hit = 1;
                            if (cw->maximized) {
                                cw->x = cw->saved_x;
                                cw->y = cw->saved_y;
                                cw->w = cw->saved_w;
                                cw->h = cw->saved_h;
                                cw->maximized = 0;
                            } else {
                                cw->saved_x = cw->x;
                                cw->saved_y = cw->y;
                                cw->saved_w = cw->w;
                                cw->saved_h = cw->h;
                                cw->x = 0;
                                cw->y = 18;            /* below top status bar */
                                cw->w = (int)ctx.width;
                                cw->h = (int)ctx.height - 18 - TASKBAR_H;
                                cw->maximized = 1;
                            }
                            /* Still raise + focus so the click
                             * brings the maximized window to top. */
                            g_z_counter++;
                            cw->raised = g_z_counter;
                            focused = hit;
                        } else if (ms.x >= mnbx && ms.x < mnbx + 14) {
                            /* minimize */
                            button_hit = 1;
                            cw->minimized = 1;
                            if (focused == hit) focused = -1;
                        }
                    }
                }
                if (!button_hit) {
                    /* Raise. */
                    g_z_counter++;
                    g_windows[hit].raised = g_z_counter;
                    focused = hit;
                    /* Session 131 — resize grip wins over title-bar
                     * drag because the grip can lie behind/inside
                     * a title bar's bbox on tiny windows.  We test
                     * the grip first. */
                    if (in_resize_grip(&g_windows[hit], ms.x, ms.y)) {
                        g_resize_idx       = hit;
                        g_resize_anchor_w  = g_windows[hit].w;
                        g_resize_anchor_h  = g_windows[hit].h;
                        g_resize_anchor_mx = ms.x;
                        g_resize_anchor_my = ms.y;
                    } else if (in_titlebar(&g_windows[hit], ms.x, ms.y)) {
                        /* Start drag if in title bar (but not in
                         * the close box, already excluded above). */
                        g_drag_idx   = hit;
                        g_drag_off_x = ms.x - g_windows[hit].x;
                        g_drag_off_y = ms.y - g_windows[hit].y;
                    }
                }
            } else {
                focused = -1;
            }
        after_press_hit:;
        }
        /* Session 117 — push FOCUS / UNFOCUS edges to client
         * windows when the *click-focused* window changes.  Use
         * client_id to identify the window across slot-array
         * compaction (drain_wm_messages can shift indices). */
        unsigned int new_focus_id =
            (focused >= 0 && focused < g_window_count
             && g_windows[focused].kind == KIND_CLIENT)
            ? g_windows[focused].client_id : 0u;
        if ((unsigned int)new_focus_id != (unsigned int)prev_focus_id) {
            if (prev_focus_id != 0) {
                struct sys_wm_event ev = {0};
                ev.type = WM_EV_UNFOCUS;
                sys_wm_event_push(prev_focus_id, &ev);
            }
            if (new_focus_id != 0) {
                struct sys_wm_event ev = {0};
                ev.type = WM_EV_FOCUS;
                sys_wm_event_push(new_focus_id, &ev);
            }
            prev_focus_id = new_focus_id;
        }
        if (released) {
            g_drag_idx   = -1;
            g_resize_idx = -1;
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
        /* Session 131 — resize-in-progress.  Outer (w, h) tracks
         * the cursor delta from the anchor; clamp to a sane min
         * and to the FB bounds. */
        if (g_resize_idx >= 0) {
            struct window *w = &g_windows[g_resize_idx];
            int nw = g_resize_anchor_w + (ms.x - g_resize_anchor_mx);
            int nh = g_resize_anchor_h + (ms.y - g_resize_anchor_my);
            if (nw < WIN_MIN_W) nw = WIN_MIN_W;
            if (nh < WIN_MIN_H) nh = WIN_MIN_H;
            if (w->x + nw > (int)ctx.width  - 2)  nw = (int)ctx.width  - 2 - w->x;
            if (w->y + nh > (int)ctx.height - 2)  nh = (int)ctx.height - 2 - w->y;
            w->w = nw;
            w->h = nh;
        }
        g_prev_left = left;

        /* Session 113 — route mouse events to client-backed windows.
         *
         *   hover changes  → FOCUS / UNFOCUS edges
         *   mouse motion   → MOUSE_MOVE (only inside the content
         *                    area, x,y translated to surface-local)
         *   press/release  → MOUSE_PRESS / MOUSE_RELEASE on the
         *                    window the cursor is over, also with
         *                    surface-local coords
         *
         * Events landing on the title bar of a CLIENT window go to
         * the WM (for drag), not the client.  Events on demo windows
         * are simply not forwarded since they aren't client-backed. */
        int hover = hit_test(ms.x, ms.y);
        int hover_is_client_content =
            (hover >= 0
             && g_windows[hover].kind == KIND_CLIENT
             && ms.y >= g_windows[hover].y + TITLE_H
             && ms.y <  g_windows[hover].y + g_windows[hover].h - 1);
        int target = hover_is_client_content ? hover : -1;

        /* Session 117 — HOVER_ENTER / HOVER_LEAVE edges when the
         * cursor crosses a client window's content area.  Distinct
         * from FOCUS / UNFOCUS (above; click-driven). */
        if (target != prev_hover_idx) {
            if (prev_hover_idx >= 0
                && prev_hover_idx < g_window_count
                && g_windows[prev_hover_idx].kind == KIND_CLIENT) {
                struct sys_wm_event ev = {0};
                ev.type = WM_EV_HOVER_LEAVE;
                sys_wm_event_push(g_windows[prev_hover_idx].client_id, &ev);
            }
            if (target >= 0) {
                struct sys_wm_event ev = {0};
                ev.type = WM_EV_HOVER_ENTER;
                sys_wm_event_push(g_windows[target].client_id, &ev);
            }
            prev_hover_idx = target;
        }

        if (target >= 0) {
            struct window *w = &g_windows[target];
            int sx = ms.x - (w->x + 1);
            int sy = ms.y - (w->y + TITLE_H);
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            if (sx >= (int)w->surface_w) sx = (int)w->surface_w - 1;
            if (sy >= (int)w->surface_h) sy = (int)w->surface_h - 1;
            /* Motion event — only when the cursor actually moved. */
            if (ms.x != prev_mx || ms.y != prev_my) {
                struct sys_wm_event ev = {0};
                ev.type = WM_EV_MOUSE_MOVE;
                ev.x    = sx;
                ev.y    = sy;
                ev.button = ms.buttons;
                sys_wm_event_push(w->client_id, &ev);
            }
            if (pressed) {
                struct sys_wm_event ev = {0};
                ev.type   = WM_EV_MOUSE_PRESS;
                ev.x      = sx;
                ev.y      = sy;
                ev.button = WM_BUTTON_LEFT;
                sys_wm_event_push(w->client_id, &ev);
            }
            if (released) {
                struct sys_wm_event ev = {0};
                ev.type   = WM_EV_MOUSE_RELEASE;
                ev.x      = sx;
                ev.y      = sy;
                ev.button = WM_BUTTON_LEFT;
                sys_wm_event_push(w->client_id, &ev);
            }
        }
        prev_mx = ms.x;
        prev_my = ms.y;

        /* Session 114 — keyboard input.  Drain any keystrokes from
         * the kernel ring and forward them to the click-focused
         * client window (`focused`, not `target` — keyboard focus
         * is click-based, not hover-based).  Bytes that arrive while
         * no client window is focused are dropped.
         *
         * Session 135 — Alt+Tab arrives as the sentinel byte 0x80
         * from the USB-HID layer.  wmd consumes it itself: cycle
         * `focused` to the next CLIENT window in registration order,
         * raising + un-minimizing as we go.  Cycling skips
         * non-client (demo) slots.  Never forwarded to clients. */
        for (int drained = 0; drained < 32; drained++) {
            int c = sys_kbd_poll();
            if (c <= 0) break;
            if (focused < 0
                || focused >= g_window_count
                || g_windows[focused].kind != KIND_CLIENT) continue;
            struct sys_wm_event ev = {0};
            ev.type    = WM_EV_KEY;
            ev.keycode = (unsigned int)c;
            sys_wm_event_push(g_windows[focused].client_id, &ev);
        }

        /* Session 135 — Alt+Tab arrives on a separate channel
         * (SYS_WM_POLL_ALTTAB) so the shell or any other reader
         * of the kbd ring can't intercept it.  Drain everything
         * pending; each press cycles `focused` to the next CLIENT
         * window in registration order, wrapping. */
        while (sys_wm_poll_alttab() > 0) {
            int start = focused;
            int next  = -1;
            for (int step = 1; step <= g_window_count; step++) {
                int idx = ((start + step) % g_window_count
                           + g_window_count) % g_window_count;
                if (g_windows[idx].kind == KIND_CLIENT) {
                    next = idx;
                    break;
                }
            }
            if (next >= 0) {
                g_windows[next].minimized = 0;
                g_z_counter++;
                g_windows[next].raised = g_z_counter;
                focused = next;
            }
        }

        /* Compose the frame.  Session 127 — procedural wallpaper
         * replaces the flat dark-blue fill from session 111. */
        paint_wallpaper(&ctx);

        /* Top status bar across the screen. */
        gfx_fill_rect(&ctx, 0, 0, (int)ctx.width, 18, GFX_DARK_GREY);
        gfx_text(&ctx, 8, 5, "wmd - AdventOS Path C session 111",
                 GFX_WHITE, GFX_TRANSPARENT);
        /* Session 124 — show the focused window's title on the
         * right side of the top bar (when there is one).  Aligned
         * to roughly column = fb_w/2 so it doesn't overlap the
         * left-side wmd label. */
        if (focused >= 0 && focused < g_window_count) {
            struct window *fw = &g_windows[focused];
            char fbuf[44];
            int n = 0;
            const char *p = "focus: ";
            while (*p && n < (int)sizeof(fbuf) - 1) fbuf[n++] = *p++;
            for (int i = 0; fw->title[i] && n < (int)sizeof(fbuf) - 1; i++)
                fbuf[n++] = fw->title[i];
            fbuf[n] = 0;
            gfx_text(&ctx, (int)ctx.width / 2 + 80, 5, fbuf,
                     GFX_CYAN, GFX_TRANSPARENT);
        }

        int order[MAX_WINDOWS];
        z_order(order);
        for (int i = 0; i < g_window_count; i++) {
            int idx = order[i];
            /* Session 133 — minimized windows aren't drawn (taskbar
             * button is still drawn; click it to restore). */
            if (g_windows[idx].minimized) continue;
            paint_window(&ctx, &g_windows[idx],
                         idx == focused, t_sec, (unsigned int)tick);
        }

        /* Session 118 — taskbar painted on top of windows so it
         * stays visible.  Window decorations can extend into the
         * taskbar region during drag; that's a UI smell but the
         * taskbar gets the last word on its strip. */
        paint_taskbar(&ctx, focused);
        /* Session 119 — launcher popup is drawn on top of the
         * taskbar when open. */
        paint_launcher(&ctx);
        /* Session 124 — right-click context menu on top of
         * everything (last paint wins). */
        paint_ctx_menu(&ctx);

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
