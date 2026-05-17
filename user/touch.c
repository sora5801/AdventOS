/*
 * touch — create empty files (or no-op on existing ones).
 *
 *   touch FILE [FILE...]
 *
 * On AdventOS there's no mtime to update — entries on the FS don't
 * carry one — so an existing FILE is a true no-op (we don't even
 * re-persist). A missing FILE is created via sys_fs_write with a
 * zero-byte payload, which adds a fresh slot with mode 0644.
 *
 * Exit code: 1 if any touch failed, 0 only if all succeeded.
 *
 * Notes for future sessions: if/when fs_entry grows an mtime field,
 * touch should bump it on the existing-file path. Right now there's
 * literally nothing to bump.
 */

#include "libuser.h"

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write(2, "usage: touch FILE [FILE...]\n", 28);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        /* sys_fs_size returns -1 if the file doesn't exist OR isn't
         * a regular file (e.g. a directory). The existing-but-not-
         * a-regular-file case lands in the create branch below, and
         * sys_fs_write will refuse (parent already exists with
         * different type). That's the right behavior — touching a
         * dir should fail. */
        if (sys_fs_size(argv[i]) >= 0) {
            continue;     /* exists as regular file — true no-op */
        }
        /* Doesn't exist — create empty. sys_fs_write with n=0 writes
         * a slot entry and zero data sectors. */
        if (sys_fs_write(argv[i], "", 0) < 0) {
            sys_write(2, "touch: cannot create '", 22);
            sys_write(2, argv[i], my_strlen(argv[i]));
            sys_write(2, "'\n", 2);
            rc = 1;
        }
    }
    return rc;
}
