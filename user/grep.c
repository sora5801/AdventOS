/*
 * grep — print lines containing a literal substring.
 *
 *   seq 30 | grep 1            -> 1, 10..19, 21
 *   grep "once" /etc/inittab   -> the two `once` lines
 *
 * Literal-only matching (no regex, no character classes). Reads input
 * line-by-line (newline-terminated), runs a naive O(line_len * pat_len)
 * substring scan on each line, prints if a match was found.
 *
 * Flags: -v inverts the match (print lines that DON'T contain pattern).
 */

#include "libuser.h"

#define LINE_MAX  512

static int contains(const char *line, int len, const char *pat) {
    int plen = (int)strlen(pat);
    if (plen == 0) return 1;
    for (int i = 0; i + plen <= len; i++) {
        int ok = 1;
        for (int j = 0; j < plen; j++) {
            if (line[i + j] != pat[j]) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static int search_fd(int fd, const char *pat, int invert) {
    char line[LINE_MAX];
    int  len = 0;
    int  hits = 0;
    char c;
    int  n;
    while ((n = sys_read(fd, &c, 1)) > 0) {
        if (len < LINE_MAX) line[len++] = c;
        if (c == '\n' || len == LINE_MAX) {
            int has = contains(line, len, pat);
            if (has != invert) {                /* xor: invert flips it */
                sys_write(1, line, len);
                hits++;
            }
            len = 0;
        }
    }
    if (len > 0) {
        int has = contains(line, len, pat);
        if (has != invert) {
            sys_write(1, line, len);
            hits++;
        }
    }
    return hits;
}

int main(int argc, char **argv) {
    int invert = 0;
    int argi   = 1;

    if (argi < argc && strcmp(argv[argi], "-v") == 0) { invert = 1; argi++; }

    if (argi >= argc) {
        sys_write(2, "grep: usage: grep [-v] PATTERN [FILE...]\n", 41);
        return 2;
    }

    const char *pat = argv[argi++];

    if (argi >= argc) {
        return (search_fd(0, pat, invert) > 0) ? 0 : 1;
    }

    int total = 0;
    for (int i = argi; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "grep: cannot open ", 18);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, "\n", 1);
            continue;
        }
        total += search_fd(fd, pat, invert);
        sys_close(fd);
    }
    return (total > 0) ? 0 : 1;
}
