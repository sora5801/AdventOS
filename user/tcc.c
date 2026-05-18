/*
 * Session 137 — tcc UX wrapper.
 *
 * Ships as /tcc.elf.  Exec's the underlying /tccraw.elf binary with
 * AdventOS-default flags prepended, so the user can run
 *
 *     tcc /hello.c -o /hello.elf
 *
 * on a stock source with `#include <stdio.h>` + `int main(void) { ... }`
 * and get a working binary.
 *
 * Defaults injected when the user is NOT preprocessing (-E), NOT
 * stopping at -c (compile-to-object), and NOT passing -nostdlib /
 * -nostartfiles themselves (escape hatches for advanced use):
 *
 *   -static -nostdlib -Wl,-Ttext=0x40000000
 *   /tcc/lib/start.c          (provides _start)
 *   /tcc/lib/libuser.c        (provides printf, malloc, etc.)
 *
 * tcc.elf was cross-built with -DCONFIG_TCCDIR='"/tcc"' so the system
 * header path /tcc/include is already on tcc's include search list.
 *
 * The wrapper also adds -lkernel-style flags absent — instead the
 * full source of libuser is recompiled into every user program. That
 * is fast enough for a 1500-line file inside a 64MB VM and avoids
 * shipping a tcc-emitted libuser.o.
 */
#include "libuser.h"

#define MAX_ARGV   256

int main(int argc, char **argv) {
    /* Scan the user's argv for the flags that suppress default linking. */
    int user_wants_link = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        /* Exact-string match is sufficient — tcc options don't have
         * value-attached forms for these. */
        if (a[0] == '-' && a[1] == 'c' && a[2] == 0) user_wants_link = 0;
        else if (a[0] == '-' && a[1] == 'E' && a[2] == 0) user_wants_link = 0;
        else if (strcmp(a, "-v") == 0) user_wants_link = 0;
        else if (strcmp(a, "--version") == 0) user_wants_link = 0;
        else if (strcmp(a, "-h") == 0) user_wants_link = 0;
        else if (strcmp(a, "-hh") == 0) user_wants_link = 0;
        else if (strcmp(a, "-nostdlib") == 0) user_wants_link = 0;
        else if (strcmp(a, "-nostartfiles") == 0) user_wants_link = 0;
        else if (strcmp(a, "-r") == 0) user_wants_link = 0;
    }

    const char *new_argv[MAX_ARGV];
    int n = 0;
    new_argv[n++] = "/tccraw.elf";

    if (user_wants_link) {
        /* AdventOS-default linker settings. -nostdlib here is internal
         * to tcc's logic — it tells the in-tcc driver "don't add
         * Linux crt1.o / libc.a"; we substitute our own start.c +
         * libuser.c by listing them as plain source files below. */
        new_argv[n++] = "-static";
        new_argv[n++] = "-nostdlib";
        new_argv[n++] = "-Wl,-Ttext=0x40000000";
        new_argv[n++] = "/tcc/lib/start.c";
        new_argv[n++] = "/tcc/lib/libuser.c";
    }

    /* Pass through every user arg verbatim. */
    for (int i = 1; i < argc && n < MAX_ARGV - 1; i++) {
        new_argv[n++] = argv[i];
    }
    new_argv[n] = (const char *)0;

    /* sys_exec replaces this address space — never returns on success. */
    sys_exec("/tccraw.elf", new_argv);
    /* Fall through only on exec failure. */
    puts("tcc: could not exec /tccraw.elf");
    return 1;
}
