/*
 * man — read a man page from /man/<topic>.
 *
 *   man TOPIC          show the page for TOPIC (e.g. `man cp`)
 *   man -k WORD        list every page whose name contains WORD
 *   man                (no arg) list every available topic
 *
 * AdventOS doesn't bother with the Unix man section numbers
 * (man 1 cp, man 3 printf). All pages live in a single flat /man/
 * directory; if a name collision ever happens (e.g., a future
 * `read` builtin AND a `read` syscall doc), we'll add a section
 * suffix at that point. For now, /man/<topic> is canonical.
 *
 * Pagination: deliberately omitted in this first cut. Pages are
 * kept short enough (typically 30-60 lines, max 80 cols) that
 * scrolling off the top of an 80x25 console isn't catastrophic.
 * A future session can layer a `less`-style pager between read
 * and write here without changing the page format.
 *
 * Exit codes:
 *   0  page found and printed (or list emitted with -k / no-arg)
 *   1  topic not found
 *   2  usage / I/O error
 */

#include "libuser.h"

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void say(int fd, const char *s) {
    sys_write(fd, s, my_strlen(s));
}

/* Substring match — returns 1 if `needle` appears anywhere in
 * `hay`. Used by -k. Empty needle matches everything. */
static int contains(const char *hay, const char *needle) {
    int nl = my_strlen(needle);
    if (nl == 0) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* Build "/man/<topic>" into out. Returns 0 on success, -1 if the
 * topic name has any slash (security: refuse path traversal). */
static int build_man_path(const char *topic, char *out, int cap) {
    const char *prefix = "/man/";
    int pi = 0;
    while (prefix[pi]) {
        if (pi + 1 >= cap) return -1;
        out[pi] = prefix[pi];
        pi++;
    }
    int ti = 0;
    while (topic[ti]) {
        if (topic[ti] == '/') return -1;
        if (pi + 1 >= cap) return -1;
        out[pi++] = topic[ti++];
    }
    out[pi] = 0;
    return 0;
}

/* Dump /man/<topic> to stdout. Returns 0 on success, 1 if the
 * topic doesn't exist, 2 on a read error mid-stream. */
static int show_page(const char *topic) {
    char path[80];
    if (build_man_path(topic, path, sizeof(path)) < 0) {
        say(2, "man: invalid topic name '");
        say(2, topic);
        say(2, "'\n");
        return 2;
    }
    int fd = sys_open(path);
    if (fd < 0) {
        say(2, "man: no manual entry for '");
        say(2, topic);
        say(2, "' (looked in ");
        say(2, path);
        say(2, ")\n");
        return 1;
    }
    char buf[512];
    for (;;) {
        int n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        sys_write(1, buf, n);
    }
    sys_close(fd);
    return 0;
}

/* Walk /man/ and print one entry per line. If `filter` is non-NULL
 * and non-empty, only print entries whose name contains the
 * filter substring (man -k behavior). */
static int list_pages(const char *filter) {
    int  iter = 0;
    char name[17];
    int  shown = 0;
    for (;;) {
        for (int i = 0; i < 17; i++) name[i] = 0;
        int idx = sys_readdir("/man", &iter, name);
        if (idx < 0) break;
        if (filter && !contains(name, filter)) continue;
        say(1, name);
        say(1, "\n");
        shown++;
    }
    if (shown == 0 && filter && filter[0]) {
        say(2, "man: -k: no entries match '");
        say(2, filter);
        say(2, "'\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        /* No args: list every page. */
        return list_pages(0);
    }
    if (argc == 3) {
        /* `man -k WORD`. */
        const char *flag = argv[1];
        if (flag[0] == '-' && flag[1] == 'k' && flag[2] == 0) {
            return list_pages(argv[2]);
        }
        say(2, "usage: man [TOPIC] | man -k WORD\n");
        return 2;
    }
    if (argc == 2) {
        const char *arg = argv[1];
        if (arg[0] == '-' && arg[1] == 'h' && arg[2] == 0) {
            say(1, "usage: man [TOPIC]    show the page for TOPIC\n");
            say(1, "       man -k WORD    list pages whose name contains WORD\n");
            say(1, "       man            list every available topic\n");
            return 0;
        }
        return show_page(arg);
    }
    say(2, "usage: man [TOPIC] | man -k WORD\n");
    return 2;
}
