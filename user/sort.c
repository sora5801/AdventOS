/*
 * sort — read lines, sort them, write them.
 *
 *   printf "c\nb\na\n" | sort        -> a b c
 *   sort /etc/inittab | uniq         -> sorted unique-ified inittab
 *
 * Implementation:
 *   - Slurp every line into a fixed-size table (capped at MAX_LINES).
 *   - Insertion sort by byte-wise compare with strcmp semantics
 *     (NUL-padded; \n at the tail counts in the comparison).
 *   - Print in order.
 *
 * No `-r` (reverse), `-n` (numeric), `-u` (unique) — pipe through
 * `tac` (we don't have it) / `awk` / `uniq` for those. The simple
 * lex order suffices for everything our shell pipelines exercise.
 */

#include "libuser.h"

#define MAX_LINES  64
#define LINE_MAX   256

static char g_lines[MAX_LINES][LINE_MAX];
static int  g_len  [MAX_LINES];
static int  g_n;

static void slurp(int fd) {
    char c;
    int  n;
    int  cur_len = 0;
    while (g_n < MAX_LINES && (n = sys_read(fd, &c, 1)) > 0) {
        if (cur_len < LINE_MAX) g_lines[g_n][cur_len++] = c;
        if (c == '\n' || cur_len == LINE_MAX) {
            g_len[g_n] = cur_len;
            g_n++;
            cur_len = 0;
        }
    }
    if (cur_len > 0 && g_n < MAX_LINES) {
        g_len[g_n] = cur_len;
        g_n++;
    }
}

static int line_cmp(int i, int j) {
    int li = g_len[i], lj = g_len[j];
    int m  = (li < lj) ? li : lj;
    for (int k = 0; k < m; k++) {
        unsigned char a = (unsigned char)g_lines[i][k];
        unsigned char b = (unsigned char)g_lines[j][k];
        if (a != b) return (int)a - (int)b;
    }
    return li - lj;
}

static void swap_lines(int i, int j) {
    char tmp[LINE_MAX];
    int  tlen = g_len[i];
    for (int k = 0; k < tlen; k++) tmp[k] = g_lines[i][k];
    g_len[i] = g_len[j];
    for (int k = 0; k < g_len[j]; k++) g_lines[i][k] = g_lines[j][k];
    g_len[j] = tlen;
    for (int k = 0; k < tlen; k++) g_lines[j][k] = tmp[k];
}

int main(int argc, char **argv) {
    if (argc < 2) {
        slurp(0);
    } else {
        for (int i = 1; i < argc; i++) {
            int fd = sys_open(argv[i]);
            if (fd < 0) {
                sys_write(2, "sort: cannot open ", 18);
                sys_write(2, argv[i], (int)strlen(argv[i]));
                sys_write(2, "\n", 1);
                continue;
            }
            slurp(fd);
            sys_close(fd);
        }
    }

    /* Insertion sort. Lines table is small so O(n^2) is fine. */
    for (int i = 1; i < g_n; i++) {
        int j = i;
        while (j > 0 && line_cmp(j - 1, j) > 0) {
            swap_lines(j - 1, j);
            j--;
        }
    }

    for (int i = 0; i < g_n; i++) sys_write(1, g_lines[i], g_len[i]);
    return 0;
}
