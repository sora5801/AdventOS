/*
 * uniq — collapse adjacent duplicate lines.
 *
 *   printf "a\na\nb\na\n" | uniq      -> a b a
 *   sort foo | uniq                  -> all unique lines (because
 *                                        sort makes equals adjacent)
 *
 * Strictly adjacent dedup — we don't see the lines that came earlier,
 * so `a b a` keeps both `a`s. POSIX-correct; if you want global
 * uniq, run `sort | uniq`.
 *
 * Flags: none today. (-c for count would just bloat the binary.)
 */

#include "libuser.h"

#define LINE_MAX  512

int main(int argc, char **argv) {
    int fd = 0;
    if (argc >= 2) {
        fd = sys_open(argv[1]);
        if (fd < 0) {
            sys_write(2, "uniq: cannot open ", 18);
            sys_write(2, argv[1], (int)strlen(argv[1]));
            sys_write(2, "\n", 1);
            return 1;
        }
    }

    char prev[LINE_MAX]; int prev_len = 0; int have_prev = 0;
    char cur [LINE_MAX]; int cur_len  = 0;

    char c;
    int  n;
    while ((n = sys_read(fd, &c, 1)) > 0) {
        if (cur_len < LINE_MAX) cur[cur_len++] = c;
        if (c == '\n' || cur_len == LINE_MAX) {
            int dup = (have_prev && cur_len == prev_len &&
                       memcmp(cur, prev, cur_len) == 0);
            if (!dup) {
                sys_write(1, cur, cur_len);
                for (int i = 0; i < cur_len; i++) prev[i] = cur[i];
                prev_len = cur_len;
                have_prev = 1;
            }
            cur_len = 0;
        }
    }
    if (cur_len > 0) {
        int dup = (have_prev && cur_len == prev_len &&
                   memcmp(cur, prev, cur_len) == 0);
        if (!dup) sys_write(1, cur, cur_len);
    }

    if (fd != 0) sys_close(fd);
    return 0;
}
