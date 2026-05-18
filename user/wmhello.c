/*
 * wmhello.c — Path C phase 6/7 sample WM client.
 *
 *   wmhello [seconds]    open a 220x140 window and animate inside,
 *                        responding to mouse hover, click, and
 *                        drag-paint.  Exit after SECONDS
 *                        (default 10).
 *
 * Demonstrates the session-112 protocol (shared-memory surface) +
 * session-113 event routing.  Reactions:
 *
 *   FOCUS / UNFOCUS    border color flips green ↔ grey
 *   MOUSE_MOVE         tiny crosshair drawn at the cursor
 *   MOUSE_PRESS        background palette advances; click point
 *                      drawn as a yellow dot
 *   MOUSE_RELEASE      click point drawn as a green dot
 *
 *     struct wm_window w;
 *     wm_open(&w, "title", 220, 140);
 *     for (;;) {
 *         struct wm_event ev;
 *         while (wm_poll_event(&w, &ev)) handle(ev);
 *         repaint(&w);
 *         wm_present(&w);
 *     }
 *     wm_close(&w);
 *
 * The compositor (`wmd`) must be running first.  If `sys_wm_create`
 * fails we print a hint and exit with status 1.
 */

#include "libuser.h"
#include "../libwm/libwm.h"

static int my_atoi_str(const char *s) { return atoi(s); }

/* Distinct palette steps so a click advances the bg in a visually
 * recognizable way (the smoke test relies on this). */
static const unsigned int g_bg_palette[] = {
    0x101030u, 0x301030u, 0x103030u, 0x303010u, 0x103010u,
};
#define BG_PALETTE_N (int)(sizeof(g_bg_palette) / sizeof(g_bg_palette[0]))

int main(int argc, char **argv) {
    int seconds = 10;
    if (argc >= 2) seconds = my_atoi_str(argv[1]);
    if (seconds <= 0) seconds = 10;

    struct wm_window win;
    if (wm_open(&win, "wmhello", 220, 140) < 0) {
        printf("wmhello: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmhello: window id=%u pixels=0x%x\n", win.id,
           (unsigned int)(uintptr_t)win.pixels);

    int total_ticks = seconds * 30;
    int sq_x = 0;
    int dir  = 1;

    /* Session 113 — live input state. */
    int has_focus = 0;
    int bg_idx    = 0;
    int mx = -1, my = -1;
    int last_press_x = -1, last_press_y = -1;
    int last_press_was_release = 0;
    int press_count    = 0;
    int release_count  = 0;
    int move_count     = 0;
    int focus_edges    = 0;
    int closed         = 0;

    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        /* Drain any events that landed since last frame. */
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                /* Session 117 — react to the HOVER edges (mouse
                 * crossing the window) so wmhello's border tracks
                 * pointer position the way it did before HOVER and
                 * FOCUS were separated.  WM_EV_FOCUS is now the
                 * click-focus signal — also count those so the
                 * smoke test can confirm both flows still arrive. */
                case WM_EV_HOVER_ENTER:  has_focus = 1; focus_edges++; break;
                case WM_EV_HOVER_LEAVE:  has_focus = 0; focus_edges++; break;
                case WM_EV_FOCUS:        focus_edges++; break;
                case WM_EV_UNFOCUS:      focus_edges++; break;
                case WM_EV_MOUSE_MOVE:
                    mx = ev.x; my = ev.y; move_count++;
                    break;
                case WM_EV_MOUSE_PRESS:
                    last_press_x = ev.x; last_press_y = ev.y;
                    last_press_was_release = 0;
                    bg_idx = (bg_idx + 1) % BG_PALETTE_N;
                    press_count++;
                    break;
                case WM_EV_MOUSE_RELEASE:
                    last_press_x = ev.x; last_press_y = ev.y;
                    last_press_was_release = 1;
                    release_count++;
                    break;
                case WM_EV_CLOSE:
                    closed = 1;
                    break;
                default: break;
            }
        }

        unsigned int bg = g_bg_palette[bg_idx];
        wm_clear(&win, bg);

        /* Top blue band, fixed colour so the smoke test can find it. */
        wm_fill_rect(&win, 0, 0, (int)win.w, 18, 0x4080E0u);

        /* Bouncing red square. */
        sq_x += dir * 3;
        if (sq_x > (int)win.w - 40) { sq_x = (int)win.w - 40; dir = -1; }
        if (sq_x < 0)               { sq_x = 0;                dir =  1; }
        wm_fill_rect(&win, sq_x, 40, 36, 36, 0xE03030u);
        wm_fill_rect(&win, sq_x + 8, 48, 20, 20, 0xFFFFFFu);

        /* Bottom border colour-codes focus state. */
        unsigned int border = has_focus ? 0x30E030u : 0x606060u;
        wm_fill_rect(&win, 0, (int)win.h - 4, (int)win.w, 4, border);

        /* Last click marker — yellow if press, green if release. */
        if (last_press_x >= 0) {
            unsigned int dot = last_press_was_release ? 0x30E030u
                                                       : 0xE0E030u;
            wm_fill_rect(&win, last_press_x - 4, last_press_y - 4,
                         9, 9, dot);
        }

        /* Cursor crosshair (only when we have focus). */
        if (has_focus && mx >= 0 && my >= 0) {
            for (int dx = -5; dx <= 5; dx++)
                wm_put_pixel(&win, mx + dx, my, 0xFFFFFFu);
            for (int dy = -5; dy <= 5; dy++)
                wm_put_pixel(&win, mx, my + dy, 0xFFFFFFu);
        }

        wm_present(&win);
        sys_sleep_ms(33);     /* ~30 fps */
    }

    printf("wmhello: focus=%d move=%d press=%d release=%d\n",
           focus_edges, move_count, press_count, release_count);
    wm_close(&win);
    return 0;
}
