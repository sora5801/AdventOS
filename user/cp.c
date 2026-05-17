/*
 * cp — copy a regular file.
 *
 *   cp SRC DST    copy SRC's contents to DST (DST is truncated/created)
 *
 * Two-argument form only; no `cp SRC... DSTDIR` because that needs
 * dirname extraction and per-target path construction that doesn't
 * pull its weight for the minimal-coreutils pass. No -r, no -p
 * (preserve mode), no -i. The kernel write goes through SYS_FS_WRITE
 * which atomically allocates a new sector run before freeing the old
 * one, so an interrupted cp leaves DST either unchanged or fully
 * replaced — never half-overwritten.
 *
 * Memory model: we read the whole SRC into one user-side buffer via
 * sys_brk. That's the simplest correct shape — the on-disk write
 * needs a single contiguous user pointer anyway (sector-by-sector
 * read happens kernel-side inside vfs_write_all). The FS hard-caps
 * file size by the contiguous sector-run allocator, so a
 * single-allocation buffer is safe up to whatever the FS allows.
 *
 * Exit code: 0 on success, 1 on any failure.
 */

#include "libuser.h"

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void say_err(const char *who, const char *path) {
    sys_write(2, "cp: ", 4);
    sys_write(2, who, my_strlen(who));
    sys_write(2, " '", 2);
    sys_write(2, path, my_strlen(path));
    sys_write(2, "'\n", 2);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        sys_write(2, "usage: cp SRC DST\n", 18);
        return 1;
    }
    const char *src = argv[1];
    const char *dst = argv[2];

    int size = sys_fs_size(src);
    if (size < 0) { say_err("cannot stat", src); return 1; }

    /* Bump heap to hold `size` bytes. heap_brk grows on demand; we
     * start from wherever the runtime left it. The libuser malloc is
     * a separate allocator but for one big slab a direct sys_brk call
     * is cleaner and avoids the malloc free-list bookkeeping. */
    int base = sys_brk(0);
    int want = base + size;
    if (size > 0 && sys_brk(want) != want) {
        sys_write(2, "cp: out of memory\n", 18);
        return 1;
    }
    char *buf = (char *)(uint32_t)base;

    /* Read src in one or more chunks (the kernel sys_read for FS files
     * can return less than requested, but in practice fills up to a
     * full sector each call — loop defensively). */
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
    return 0;
}
