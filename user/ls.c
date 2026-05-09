/*
 * ls — list directory entries, one per line.
 *
 *   ls                -> contents of cwd
 *   ls /etc           -> contents of /etc
 *   ls /etc | wc -l   -> count entries via pipeline
 *
 * Why a binary AND a shell builtin? Because shell builtins can't
 * appear in pipelines (they'd run inline in the shell instead of in
 * a forked child). The builtin is for snappy interactive use; this
 * binary is what `ls /etc | wc -l` actually exec()s into.
 *
 * No flags today: no -l, no -a, no -1 (we always list one per line).
 */

#include "libuser.h"

int main(int argc, char **argv) {
    /* Resolve the path argument. Bare or "." means cwd, fetched
     * via SYS_GETCWD because SYS_READDIR takes a path string. */
    char  cwd_buf[128];
    const char *path = ".";
    if (argc >= 2) path = argv[1];

    if (path[0] == '.' && path[1] == 0) {
        if (sys_getcwd(cwd_buf, sizeof(cwd_buf)) < 0) {
            sys_write(2, "ls: getcwd failed\n", 18);
            return 1;
        }
        path = cwd_buf;
    }

    int  iter = 0;
    char name[17];
    int  shown = 0;
    for (;;) {
        /* Pre-zero so we can append \n at the right spot regardless
         * of whether the kernel filled all 16 slots (the on-disk
         * name is 16 bytes NUL-padded; SYS_READDIR doesn't append
         * a 17th NUL when the name is exactly 16 chars). */
        for (int i = 0; i < 17; i++) name[i] = 0;
        int idx = sys_readdir(path, &iter, name);
        if (idx < 0) break;
        sys_write(1, name, (int)strlen(name));
        sys_write(1, "\n", 1);
        shown++;
    }

    if (shown == 0) {
        /* Distinguishing "empty dir" from "bad path" requires a
         * separate sys_open + fs_entry_type — we don't have an
         * is_dir helper exposed to userspace. Plain message either
         * way; the t17 tests cover the non-empty path. */
        sys_write(2, "ls: empty or no such directory\n", 31);
        return 1;
    }
    return 0;
}
