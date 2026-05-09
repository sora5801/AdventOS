/*
 * AdventOS userspace shell — pid 1 after boot.
 *
 * Reads a line via SYS_READ_LINE (kernel handles keyboard / serial
 * echo), then either:
 *   - handles a small set of commands locally in user mode, or
 *   - forwards the line to SYS_KCMD which runs it through the
 *     kernel's existing command parser.
 *
 * Local commands exist mainly to demonstrate that real ring-3 logic
 * is happening, not just a thin pipe to the kernel.
 */

#include "libuser.h"

static const char *g_prompt = "advent$ ";

static void cmd_uhelp(void) {
    puts("Userspace shell (running in ring 3 as pid 1):\n");
    puts("  Local commands (run in user mode):\n");
    puts("    uhelp        - this list\n");
    puts("    upid         - print pid via SYS_GETPID\n");
    puts("    utime        - print epoch via SYS_TIME\n");
    puts("    usleep MS    - sleep, watch other tasks run\n");
    puts("    uexit        - exit shell (kernel halts shortly after)\n");
    puts("\n");
    puts("  Anything else is forwarded to SYS_KCMD. Try 'help' to\n");
    puts("  see what the kernel offers (tasks, ifconfig, ping, ls,\n");
    puts("  exec, meminfo, ...).\n");
}

static void cmd_usleep(const char *arg) {
    uint32_t ms = 0;
    while (*arg >= '0' && *arg <= '9') { ms = ms * 10 + (*arg - '0'); arg++; }
    if (ms == 0) { puts("usleep: usage: usleep <ms>\n"); return; }
    uint32_t before = sys_time();
    sys_sleep_ms(ms);
    uint32_t after = sys_time();
    printf("usleep: slept %u ms (epoch %u -> %u)\n", ms, before, after);
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

int main(void) {
    puts("\n");
    puts("AdventOS userspace shell, pid="); printf("%d\n", sys_getpid());
    puts("Type 'uhelp' for shell-local commands, 'help' for kernel ones.\n\n");

    char line[256];
    for (;;) {
        puts(g_prompt);
        int n = sys_read_line(line, sizeof(line));
        if (n <= 0) continue;

        /* Local commands first. */
        if (strcmp(line, "uhelp") == 0)        { cmd_uhelp(); continue; }
        if (strcmp(line, "upid")  == 0)        { printf("user pid: %d\n", sys_getpid()); continue; }
        if (strcmp(line, "utime") == 0)        { printf("epoch: %u\n",   sys_time());   continue; }
        if (strcmp(line, "uexit") == 0)        { puts("bye\n"); sys_exit(0); }
        if (starts_with(line, "usleep "))      { cmd_usleep(line + 7); continue; }

        /* Everything else: hand to the kernel. */
        sys_kcmd(line);
    }
}
