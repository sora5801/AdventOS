/*
 * mv — move/rename a regular file.
 *
 *   mv SRC DST    rename SRC to DST
 *
 * AdventFS doesn't expose a single rename syscall, so this is
 * implemented as copy-then-unlink. That's atomicity-equivalent to a
 * real rename for our purposes — sys_fs_write atomically swaps the
 * file's sector run (new run allocated before old is freed), and
 * sys_unlink runs after the new file is fully durable. If the unlink
 * fails (e.g., DST landed on the FS but SRC was held open by another
 * task), we leave BOTH files in place and return 1; the user can then
 * remove either explicitly. Better than the rename-and-lose-data case.
 *
 * Two-argument form only — no `mv SRC... DSTDIR` (dirname extraction).
 * Refuses if DST already exists (POSIX `mv` without -f would prompt;
 * we just fail to keep the behavior predictable for scripts).
 *
 * Exit code: 0 on success, 1 on any failure.
 */

#include "libuser.h"

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void say_err(const char *who, const char *path) {
    sys_write(2, "mv: ", 4);
    sys_write(2, who, my_strlen(who));
    sys_write(2, " '", 2);
    sys_write(2, path, my_strlen(path));
    sys_write(2, "'\n", 2);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        sys_write(2, "usage: mv SRC DST\n", 18);
        return 1;
    }
    const char *src = argv[1];
    const char *dst = argv[2];

    /* Atomic rename first — works for both paths under /mnt/9p where
     * the 9P driver issues Trenameat.  Other filesystems return -1
     * and we fall through to the copy+unlink path below. */
    if (sys_rename(src, dst) == 0) {
        return 0;
    }

    if (sys_fs_size(dst) >= 0) {
        say_err("destination exists:", dst);
        return 1;
    }

    int size = sys_fs_size(src);
    if (size < 0) { say_err("cannot stat", src); return 1; }

    /* Same single-slab buffer strategy as cp. */
    int base = sys_brk(0);
    int want = base + size;
    if (size > 0 && sys_brk(want) != want) {
        sys_write(2, "mv: out of memory\n", 18);
        return 1;
    }
    char *buf = (char *)(uint32_t)base;

    int fd = sys_open(src);
    if (fd < 0) { say_err("cannot open", src); return 1; }
    int got = 0;
    while (got < size) {
        int n = sys_read(fd, buf + got, size - got);
        if (n <= 0) break;
        got += n;
    }
    sys_close(fd);
    if (got != size) { say_err("short read on", src); return 1; }

    if (sys_fs_write(dst, buf, (uint32_t)size) < 0) {
        say_err("cannot write", dst);
        return 1;
    }
    if (sys_unlink(src) < 0) {
        /* Copy succeeded but unlink failed — leave both, the user
         * has more info than we do about why. */
        say_err("copy succeeded but cannot unlink", src);
        return 1;
    }
    return 0;
}
