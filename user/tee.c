/*
 * tee — copy stdin to stdout AND to one or more named files.
 *
 *   seq 5 | tee /save.txt | wc -l   -> 5  (and /save.txt holds 1..5)
 *
 * The output files are opened with sys_open_w (the in-RAM tmpfs that
 * powers the shell's `>` redirection), so they live for the session
 * and disappear on reboot. Multiple files are supported — each gets
 * the same byte stream. We don't propagate write errors as exit
 * codes; partial writes happen in real tee and we just push through.
 *
 * Flag -a (append) would need persistent open semantics we haven't
 * built; today every file is truncated on open.
 */

#include "libuser.h"

#define MAX_OUT  4

int main(int argc, char **argv) {
    int  fds[MAX_OUT];
    int  nfds = 0;

    for (int i = 1; i < argc && nfds < MAX_OUT; i++) {
        /* Session 82: silently consume --advjson — tee is byte-
         * transparent so the flag is a no-op. We have to recognise
         * it explicitly though, otherwise the argv-injection path
         * would have us trying to open a file named `--advjson`
         * and erroring out, surprising the agent. */
        if (strcmp(argv[i], "--advjson") == 0) continue;
        int fd = sys_open_w(argv[i]);
        if (fd < 0) {
            sys_write(2, "tee: cannot open ", 17);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, "\n", 1);
            continue;
        }
        fds[nfds++] = fd;
    }

    char buf[256];
    int  n;
    while ((n = sys_read(0, buf, sizeof(buf))) > 0) {
        sys_write(1, buf, n);
        for (int i = 0; i < nfds; i++) sys_write(fds[i], buf, n);
    }

    for (int i = 0; i < nfds; i++) sys_close(fds[i]);
    return 0;
}
