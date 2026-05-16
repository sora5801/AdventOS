/*
 * tail — print the last N lines (default 10) of stdin or a named file.
 *
 *   seq 100 | tail            -> 91..100
 *   seq 100 | tail -3         -> 98..100
 *   tail -n 5 /etc/inittab    -> last 5 lines
 *
 * Implemented as a fixed-size circular buffer of complete lines. We
 * have to read the whole input — there's no way to know which lines
 * are last without seeing all of them. Lines are capped at LINE_MAX
 * bytes; longer lines get split (the overflow becomes its own "line"
 * in the buffer, same as GNU tail in a tight buffer).
 */

#include "libuser.h"

#define MAX_LINES   64
#define LINE_MAX    256

static char     g_lines[MAX_LINES][LINE_MAX];
static int      g_len  [MAX_LINES];
static int      g_head;       /* write index */
static int      g_count;      /* lines stored, capped at MAX_LINES */
static char     g_cur[LINE_MAX];
static int      g_cur_len;

static void store_current_line(void) {
    int slot = g_head;
    g_len[slot] = g_cur_len;
    for (int i = 0; i < g_cur_len; i++) g_lines[slot][i] = g_cur[i];
    g_head = (g_head + 1) % MAX_LINES;
    if (g_count < MAX_LINES) g_count++;
    g_cur_len = 0;
}

static void tail_fd(int fd) {
    char c;
    int  n;
    while ((n = sys_read(fd, &c, 1)) > 0) {
        if (g_cur_len < LINE_MAX) g_cur[g_cur_len++] = c;
        if (c == '\n' || g_cur_len == LINE_MAX) store_current_line();
    }
    if (g_cur_len > 0) store_current_line();
}

static void emit(int want_lines) {
    int show = (g_count < want_lines) ? g_count : want_lines;
    int start = (g_head - show + MAX_LINES) % MAX_LINES;
    for (int i = 0; i < show; i++) {
        int s = (start + i) % MAX_LINES;
        sys_write(1, g_lines[s], g_len[s]);
    }
}

int main(int argc, char **argv) {
    int want = 10;
    int argi = 1;

    /* Session 82: silently consume --advjson — same rationale as head. */
    while (argi < argc && strcmp(argv[argi], "--advjson") == 0) argi++;

    if (argi < argc && argv[argi][0] == '-') {
        if (argv[argi][1] == 'n' && argv[argi][2] == 0) {
            argi++;
            if (argi < argc) {
                int n = atoi(argv[argi++]);
                if (n > 0) want = n;
            }
        } else if (argv[argi][1] >= '0' && argv[argi][1] <= '9') {
            int n = atoi(argv[argi] + 1);
            if (n > 0) want = n;
            argi++;
        } else {
            sys_write(2, "tail: bad flag\n", 15);
            return 1;
        }
    }

    if (want > MAX_LINES) want = MAX_LINES;

    if (argi >= argc) {
        tail_fd(0);
        emit(want);
        return 0;
    }

    /* Multi-file: collect each separately, emit each separately. */
    for (int i = argi; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "tail: cannot open ", 18);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, "\n", 1);
            continue;
        }
        g_head = 0; g_count = 0; g_cur_len = 0;
        tail_fd(fd);
        sys_close(fd);
        emit(want);
    }
    return 0;
}
