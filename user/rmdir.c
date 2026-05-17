/*
 * rmdir — remove one or more EMPTY directories.
 *
 *   rmdir DIR [DIR...]   delete each empty DIR
 *
 * Refuses non-empty directories (the kernel's fs_rmdir scans the slot
 * table for any child with parent_dir == this idx — POSIX ENOTEMPTY
 * analog). For recursive removal, use `rm` once we add -r support
 * (deliberately not in this session — `rmdir` matches POSIX defaults).
 *
 * Exit code: 1 if any removal failed, 0 only if all succeeded.
 */

#include "libuser.h"

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write(2, "usage: rmdir DIR [DIR...]\n", 26);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (sys_rmdir(argv[i]) < 0) {
            sys_write(2, "rmdir: cannot remove '", 22);
            sys_write(2, argv[i], my_strlen(argv[i]));
            sys_write(2, "' (not empty, missing, or no permission)\n", 41);
            rc = 1;
        }
    }
    return rc;
}
