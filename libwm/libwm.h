#ifndef ADVENTOS_LIBWM_H
#define ADVENTOS_LIBWM_H

/*
 * libwm — userspace wrapper around the session-112 WM client protocol.
 *
 *   struct wm_window w;
 *   if (wm_open(&w, "Hello", 200, 120) < 0) ...;
 *   for (;;) {
 *       wm_clear(&w, 0xFFFFFF);
 *       wm_fill_rect(&w, 10, 10, 80, 40, 0xE03030);
 *       wm_present(&w);
 *       sys_sleep_ms(16);
 *   }
 *   wm_close(&w);
 *
 * The surface is always packed 32-bit 0x00RRGGBB regardless of the
 * compositor's framebuffer bpp.  Pitch = w * 4.  Pixels at (x, y) =
 * w.pixels[y * w.pitch_px + x].
 *
 * No async events yet (session 113 lands input routing). For now the
 * client just paints; user input is the WM's problem.
 */

#include "../user/libuser.h"
#include "../include/types.h"

struct wm_window {
    unsigned int  id;          /* assigned by kernel  */
    unsigned int  w, h;        /* pixels              */
    unsigned int  pitch_px;    /* == w                */
    unsigned int *pixels;      /* mapped at kernel-chosen VA */
};

/* Open a new window.  Returns 0 on success, -1 if either there's no
 * WM bound or the kernel rejected the allocation.  `title` is copied
 * (truncated to 31 chars). */
int  wm_open  (struct wm_window *out, const char *title,
               unsigned int w, unsigned int h);

/* Release the window.  After this the surface VA is unmapped and the
 * `*out` struct should not be used. */
void wm_close (struct wm_window *out);

/* Pixel-pushing helpers. All clip against the surface bounds. */
void wm_put_pixel(struct wm_window *out, int x, int y, unsigned int rgb);
void wm_clear    (struct wm_window *out, unsigned int rgb);
void wm_fill_rect(struct wm_window *out, int x, int y, int w, int h,
                  unsigned int rgb);

/* Damage notification.  No-op in session 112 because the compositor
 * repaints every frame anyway, but client code should call this after
 * a batch of drawing so the API is forward-compatible with session
 * 113's damage-region work. */
void wm_present  (struct wm_window *out);

/* Session 113 — input.  Re-exports `struct sys_wm_event` and friends
 * from libuser as `struct wm_event` so client code can speak the
 * library's vocabulary rather than the raw syscall vocabulary.
 * Returns 1 if an event was returned, 0 if the queue is empty. */
struct wm_event {
    unsigned int type;      /* WM_EV_* */
    int          x;         /* surface-local */
    int          y;
    unsigned int button;
    unsigned int keycode;
};
int wm_poll_event(struct wm_window *w, struct wm_event *out);

/* Session 136 — clipboard convenience wrappers.  Forward to
 * sys_clipboard_set / sys_clipboard_get so client code can pull
 * just libwm and have everything it needs for paint + input +
 * clipboard.  Max payload 4096 bytes. */
int wm_clipboard_set(const void *buf, int len);
int wm_clipboard_get(void *buf, int cap);

/* Session 143 — post a toast notification.  Short status text
 * (e.g. "saved /tmp/foo") appears as a fading box in the bottom-
 * right of the screen for a few seconds.  Returns 0 / -1. */
int wm_notify(const char *text);

#endif
