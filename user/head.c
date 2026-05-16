/*
 * head — print the first N lines (default 10) of stdin or a named file.
 *
 *   seq 100 | head            -> 1..10
 *   seq 100 | head -3         -> 1..3
 *   head -n 5 /etc/inittab    -> first 5 lines of inittab
 *
 * Reading is byte-at-a-time so we stop the moment the Nth newline
 * arrives — important when the producer is a long-running pipe
 * (e.g. `seq 1000000 | head -3`); a buffered approach would consume
 * far past the line cap before noticing.
 */

#include "libuser.h"

static int parse_count(const char *s) {
    int n = atoi(s);
    return (n > 0) ? n : 10;
}

static void head_fd(int fd, int want_lines) {
    int  seen = 0;
    char c;
    while (seen < want_lines) {
        int n = sys_read(fd, &c, 1);
        if (n <= 0) break;
        sys_write(1, &c, 1);
        if (c == '\n') seen++;
    }
}

int main(int argc, char **argv) {
    int want = 10;
    int argi = 1;

    /* Session 82: silently consume --advjson. head is byte-transparent
     * in JSONL mode (newlines bound records the same as text lines),
     * so the flag is a no-op for us — but we have to recognise it
     * explicitly because the shell injects it as argv[1] for every
     * |> pipeline stage, and the older "bad flag" path would error
     * out on the first byte of every structured pipeline that ends
     * with head. Same pattern as tee. */
    while (argi < argc && strcmp(argv[argi], "--advjson") == 0) argi++;

    /* -N or -n N */
    if (argi < argc && argv[argi][0] == '-') {
        if (argv[argi][1] == 'n' && argv[argi][2] == 0) {
            argi++;
            if (argi < argc) want = parse_count(argv[argi++]);
        } else if (argv[argi][1] >= '0' && argv[argi][1] <= '9') {
            want = parse_count(argv[argi] + 1);
            argi++;
        } else {
            sys_write(2, "head: bad flag\n", 15);
            return 1;
        }
    }

    if (argi >= argc) {
        head_fd(0, want);
        return 0;
    }

    for (int i = argi; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "head: cannot open ", 18);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, "\n", 1);
            continue;
        }
        head_fd(fd, want);
        sys_close(fd);
    }
    return 0;
}
