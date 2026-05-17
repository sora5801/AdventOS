/*
 * rm — remove one or more regular files.
 *
 *   rm FILE [FILE...]    delete each FILE
 *
 * Refuses to remove directories (matches POSIX without -r, which we
 * don't implement). Reports per-file errors to stderr but keeps going
 * — exit code is 1 if ANY removal failed, 0 only if all succeeded.
 *
 * No -f / -i / -r flags. The kernel sys_unlink already enforces:
 *   - owner-or-root permission
 *   - EBUSY-ish refusal if any task has the file open
 *   - refusal on directories (those need `rmdir`)
 */

#include "libuser.h"

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write(2, "usage: rm FILE [FILE...]\n", 25);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (sys_unlink(argv[i]) < 0) {
            sys_write(2, "rm: cannot remove '", 19);
            sys_write(2, argv[i], my_strlen(argv[i]));
            sys_write(2, "'\n", 2);
            rc = 1;
        }
    }
    return rc;
}
