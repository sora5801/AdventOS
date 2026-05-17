/*
 * mkdir — create one or more directories.
 *
 *   mkdir DIR [DIR...]   create each DIR
 *
 * Parent must already exist (no -p). If the dir already exists, that's
 * an error (same as POSIX without -p). Each name is passed straight
 * to sys_mkdir, which appends the new entry to the FS slot table and
 * persists the superblock.
 *
 * Exit code: 1 if any create failed, 0 only if all succeeded.
 */

#include "libuser.h"

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write(2, "usage: mkdir DIR [DIR...]\n", 26);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (sys_mkdir(argv[i]) < 0) {
            sys_write(2, "mkdir: cannot create '", 22);
            sys_write(2, argv[i], my_strlen(argv[i]));
            sys_write(2, "' (exists, missing parent, or FS full)\n", 39);
            rc = 1;
        }
    }
    return rc;
}
