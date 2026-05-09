/*
 * cat — read each named file and write its contents to stdout.
 * Demonstrates SYS_OPEN/READ/WRITE/CLOSE and the new argv pipeline.
 */

#include "libuser.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write(2, "usage: cat <file> [...]\n", 24);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "cat: ", 5);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, ": cannot open\n", 14);
            continue;
        }

        char  buf[256];
        int   n;
        while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
            sys_write(1, buf, n);
        }
        sys_close(fd);
    }
    return 0;
}
