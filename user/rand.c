/*
 * rand — print N bytes of random data (hex or raw) from the kernel.
 *
 * Usage:
 *   rand            — 16 bytes, hex
 *   rand 32         — 32 bytes, hex
 *   rand 16 raw     — 16 bytes, raw (binary to stdout)
 *
 * Uses SYS_GETRANDOM: real entropy from virtio-rng if present,
 * weak fallback (xorshift on PIT ticks + RTC) otherwise. The tool
 * prints a one-line warning to stderr-via-stdout when falling back,
 * so callers know whether to trust the output for crypto.
 */
#include "libuser.h"

static void print_hex(const unsigned char *p, int n) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        char buf[2] = { hex[p[i] >> 4], hex[p[i] & 0xF] };
        sys_write(1, buf, 2);
    }
    sys_write(1, "\n", 1);
}

int main(int argc, char **argv) {
    int n = 16;
    int raw = 0;

    if (argc >= 2) {
        n = atoi(argv[1]);
        if (n <= 0 || n > 4096) {
            puts("rand: n out of range (1..4096)\n");
            return 1;
        }
    }
    if (argc >= 3) {
        if (strcmp(argv[2], "raw") == 0) raw = 1;
    }

    static unsigned char buf[4096];
    int got = sys_getrandom(buf, n);
    /* sys_getrandom returns -1 when falling back to weak entropy
     * (no virtio-rng device). The buffer is still filled. */
    if (got < 0) {
        puts("rand: WARN — no virtio-rng device, using PIT/xorshift fallback "
             "(NOT cryptographically secure)\n");
        got = n;     /* buffer is filled anyway */
    }

    if (raw) {
        sys_write(1, buf, got);
    } else {
        print_hex(buf, got);
    }
    return 0;
}
