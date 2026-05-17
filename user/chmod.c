/*
 * chmod — change file permission bits (octal-only).
 *
 *   chmod MODE FILE [FILE...]    set each FILE's mode to MODE
 *
 * MODE is a 1-4 digit octal integer (e.g. 755, 644, 0600). Symbolic
 * modes (u+x, go-w) are intentionally not implemented — they're a
 * separate parser that doesn't pull its weight for a minimal coreutils
 * pass. The kernel sys_chmod accepts the low 9 bits as rwxrwxrwx and
 * enforces owner-or-root.
 *
 * Exit code: 1 if any chmod failed, 0 only if all succeeded.
 */

#include "libuser.h"

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

/* Parse an octal string like "755" or "0644" into an int. Returns -1
 * on any non-octal character. Caps at 4 digits to keep things sane. */
static int parse_octal(const char *s) {
    int v = 0;
    int n = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '7') return -1;
        if (++n > 4) return -1;
        v = (v << 3) | (s[i] - '0');
    }
    if (n == 0) return -1;
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        sys_write(2, "usage: chmod MODE FILE [FILE...]\n", 33);
        sys_write(2, "       MODE is octal: 755, 0644, 600, etc.\n", 44);
        return 1;
    }
    int mode = parse_octal(argv[1]);
    if (mode < 0) {
        sys_write(2, "chmod: invalid mode '", 21);
        sys_write(2, argv[1], my_strlen(argv[1]));
        sys_write(2, "' (must be octal)\n", 18);
        return 1;
    }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (sys_chmod(argv[i], mode) < 0) {
            sys_write(2, "chmod: cannot chmod '", 21);
            sys_write(2, argv[i], my_strlen(argv[i]));
            sys_write(2, "' (missing or not owner)\n", 25);
            rc = 1;
        }
    }
    return rc;
}
