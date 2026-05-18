/*
 * hvc — talk to the virtio-console port 0. Two modes:
 *
 *   hvc write "text"  — send "text" out the console
 *   hvc read          — read whatever's pending (non-blocking), print
 *   hvc echo          — loop: read input, echo back, until ^C
 *
 * Useful for verifying the virtio-console bring-up. With QEMU
 * launched as:
 *     -chardev socket,id=hvc0,host=localhost,port=5556,server=on,wait=off \
 *     -device virtio-serial-pci,id=vsbus \
 *     -device virtconsole,chardev=hvc0,bus=vsbus.0
 * the host side is reachable via `nc localhost 5556`.
 */
#include "libuser.h"

static int do_write(const char *s) {
    int len = (int)strlen(s);
    int rc = sys_virtio_console_write(s, len);
    if (rc < 0) {
        puts("hvc: no virtio-console device\n");
        return 1;
    }
    printf("hvc: wrote %d bytes\n", rc);
    return 0;
}

static int do_read(void) {
    char buf[512];
    int n = sys_virtio_console_read(buf, sizeof(buf));
    if (n < 0) {
        puts("hvc: no virtio-console device\n");
        return 1;
    }
    if (n == 0) {
        puts("(no data)\n");
        return 0;
    }
    sys_write(1, buf, n);
    sys_write(1, "\n", 1);
    return 0;
}

static int do_echo(void) {
    char buf[256];
    puts("hvc echo: reading + echoing until ^C\n");
    for (;;) {
        int n = sys_virtio_console_read(buf, sizeof(buf));
        if (n < 0) {
            puts("hvc: device went away\n");
            return 1;
        }
        if (n > 0) {
            /* Show what we got locally, and bounce it back. */
            sys_write(1, "[hvc] got: ", 11);
            sys_write(1, buf, n);
            sys_write(1, "\n", 1);
            sys_virtio_console_write(buf, n);
        }
        sys_sleep_ms(100);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: hvc {write \"text\" | read | echo}\n");
        return 1;
    }
    if (strcmp(argv[1], "write") == 0) {
        if (argc < 3) { puts("hvc write: need text arg\n"); return 1; }
        return do_write(argv[2]);
    }
    if (strcmp(argv[1], "read") == 0) return do_read();
    if (strcmp(argv[1], "echo") == 0) return do_echo();
    puts("hvc: unknown subcommand\n");
    return 1;
}
