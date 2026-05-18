/*
 * wmhello.c — session 112 / Path C phase 6 sample WM client.
 *
 *   wmhello [seconds]    open a 200x140 window, paint a moving
 *                        colored square inside, and exit after
 *                        SECONDS (default 10).
 *
 * Demonstrates the smallest possible client of the session-112 WM
 * protocol:
 *
 *     struct wm_window w;
 *     wm_open(&w, "title", 200, 140);
 *     for (;;) { wm_clear(...); wm_fill_rect(...); wm_present(&w); }
 *     wm_close(&w);
 *
 * The compositor (`wmd`) must be running first.  If `sys_wm_create`
 * fails we print a hint and exit with status 1.
 */

#include "libuser.h"
#include "../libwm/libwm.h"

static int my_atoi_str(const char *s) { return atoi(s); }

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
    for (int tick = 0; tick < total_ticks; tick++) {
        /* Background gradient that shifts color over time. */
        unsigned int bg = 0x101030u + (unsigned int)(tick & 0x3F);
        wm_clear(&win, bg);

        /* Title-ish band. */
        wm_fill_rect(&win, 0, 0, (int)win.w, 18, 0x4080E0u);

        /* Moving square. */
        sq_x += dir * 3;
        if (sq_x > (int)win.w - 40) { sq_x = (int)win.w - 40; dir = -1; }
        if (sq_x < 0)               { sq_x = 0;                dir =  1; }
        wm_fill_rect(&win, sq_x, 40, 36, 36, 0xE03030u);
        wm_fill_rect(&win, sq_x + 8, 48, 20, 20, 0xFFFFFFu);

        /* Border accent. */
        wm_fill_rect(&win, 0,             (int)win.h - 4, (int)win.w, 4,
                     0x30E030u);

        wm_present(&win);
        sys_sleep_ms(33);     /* ~30 fps */
    }

    wm_close(&win);
    printf("wmhello: done\n");
    return 0;
}
