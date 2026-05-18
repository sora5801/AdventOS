/*
 * libwm.c — userspace client library for the session-112 WM
 * protocol.  Wraps SYS_WM_CREATE / SYS_WM_DESTROY plus pixel-pushing
 * helpers.  Painting goes directly into the kernel-mapped surface;
 * no syscall per pixel.
 */

#include "libwm.h"

int wm_open(struct wm_window *out, const char *title,
            unsigned int w, unsigned int h) {
    if (!out) return -1;
    struct sys_wm_create args;
    /* Zero everything so the kernel sees a clean struct (title is
     * NUL-terminated, output fields start zero). */
    for (unsigned int i = 0; i < sizeof(args); i++)
        ((unsigned char *)&args)[i] = 0;
    args.w = w;
    args.h = h;
    if (title) {
        int i = 0;
        while (i < 31 && title[i]) { args.title[i] = title[i]; i++; }
        args.title[i] = 0;
    }
    if (sys_wm_create(&args) < 0) return -1;
    out->id       = args.id;
    out->w        = w;
    out->h        = h;
    out->pitch_px = w;
    out->pixels   = (unsigned int *)(uintptr_t)args.pixels_va;
    return 0;
}

void wm_close(struct wm_window *out) {
    if (!out || !out->id) return;
    sys_wm_destroy(out->id);
    out->id     = 0;
    out->pixels = 0;
}

void wm_put_pixel(struct wm_window *out, int x, int y, unsigned int rgb) {
    if (!out || !out->pixels) return;
    if (x < 0 || y < 0) return;
    if ((unsigned)x >= out->w || (unsigned)y >= out->h) return;
    out->pixels[(unsigned)y * out->pitch_px + (unsigned)x] = rgb & 0xFFFFFFu;
}

void wm_clear(struct wm_window *out, unsigned int rgb) {
    if (!out || !out->pixels) return;
    rgb &= 0xFFFFFFu;
    unsigned int n = out->w * out->h;
    for (unsigned int i = 0; i < n; i++) out->pixels[i] = rgb;
}

void wm_fill_rect(struct wm_window *out, int x, int y, int w, int h,
                  unsigned int rgb) {
    if (!out || !out->pixels) return;
    if (w <= 0 || h <= 0) return;
    if (x < 0)              { w += x; x = 0; }
    if (y < 0)              { h += y; y = 0; }
    if ((unsigned)x >= out->w || (unsigned)y >= out->h) return;
    if ((unsigned)(x + w) > out->w) w = (int)out->w - x;
    if ((unsigned)(y + h) > out->h) h = (int)out->h - y;
    if (w <= 0 || h <= 0) return;
    rgb &= 0xFFFFFFu;
    for (int yy = 0; yy < h; yy++) {
        unsigned int *row = out->pixels
                          + (unsigned)(y + yy) * out->pitch_px
                          + (unsigned)x;
        for (int xx = 0; xx < w; xx++) row[xx] = rgb;
    }
}

void wm_present(struct wm_window *out) {
    /* Session 112 — no-op. The compositor repaints every frame
     * regardless, and SYS_WM_DESTROY is the only client→WM signal
     * we need today. Future sessions add damage-region tracking
     * here. */
    (void)out;
}

int wm_poll_event(struct wm_window *w, struct wm_event *out) {
    if (!w || !w->id || !out) return 0;
    struct sys_wm_event ev;
    int r = sys_wm_event_poll(w->id, &ev);
    if (r <= 0) return 0;
    out->type    = ev.type;
    out->x       = ev.x;
    out->y       = ev.y;
    out->button  = ev.button;
    out->keycode = ev.keycode;
    return 1;
}

int wm_clipboard_set(const void *buf, int len) {
    return sys_clipboard_set(buf, len);
}
int wm_clipboard_get(void *buf, int cap) {
    return sys_clipboard_get(buf, cap);
}
