/*
 * pwd — print the current working directory.
 *
 *   pwd                  -> /
 *   cd /etc; pwd         -> /etc
 *   pwd | tr / -         -> "-" then "-etc"... pipelines work because
 *                            this is a real binary, not a shell builtin.
 *
 * One syscall, one print. The shell also has a `pwd` builtin (which
 * fast-paths to the same SYS_GETCWD); the binary exists so pipelines
 * containing pwd actually fork+exec into a child that reads the
 * cwd it inherited from the shell.
 */

#include "libuser.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[128];
    int  n = sys_getcwd(buf, sizeof(buf));
    if (n < 0) {
        sys_write(2, "pwd: error\n", 11);
        return 1;
    }
    sys_write(1, buf, n);
    sys_write(1, "\n", 1);
    return 0;
}
