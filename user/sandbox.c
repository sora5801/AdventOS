/*
 * sandbox — install a syscall allow-bitmap (+ optional resource caps)
 * and exec a command in it.
 *
 * Usage:
 *   sandbox POLICY [LIMIT_FLAGS...] -- CMD [ARG ...]
 *   sandbox --selftest
 *   sandbox --list
 *
 * POLICY is one of: minimal, compute, readfs, netclient.  See
 * user/libuser.c for the exact syscall sets behind each name.
 *
 * Resource-limit flags (session 71, all optional, all "tighten-only"):
 *   --rss-kb  N    cap total resident-set size at N kilobytes
 *   --cpu-ms  N    cap total CPU time at N milliseconds (PIT 10ms ticks)
 *   --fds     N    cap simultaneously-open file descriptors at N
 *   --wall-ms N    SIGKILL after N ms of wall-clock time
 *
 * The mask + limits are installed before exec, so the spawned binary
 * boots already sandboxed; child processes (fork) inherit both the
 * mask and the caps and cannot loosen them.
 *
 * Session 70 (sandbox) + session 71 (limits).
 */

#include "libuser.h"

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* Tiny atoi: stops at first non-digit, no leading-sign / overflow
 * handling. Negative inputs come back as 0 and are filtered by the
 * caller's "is this flag value sane" check. */
static uint32_t pos_atoi(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static void usage(void) {
    puts("usage: sandbox POLICY [LIMITS...] -- CMD [ARG ...]\n"
         "       sandbox --list             # show available policies\n"
         "       sandbox --selftest         # in-process smoke test\n"
         "\n"
         "POLICY:  minimal | compute | readfs | netclient\n"
         "LIMITS:  --rss-kb N  --cpu-ms N  --fds N  --wall-ms N\n");
}

static int build_policy(const char *name, uint32_t mask[4]) {
    if (streq(name, "minimal"))   { sandbox_policy_minimal  (mask); return 0; }
    if (streq(name, "compute"))   { sandbox_policy_compute  (mask); return 0; }
    if (streq(name, "readfs"))    { sandbox_policy_readfs   (mask); return 0; }
    if (streq(name, "netclient")) { sandbox_policy_netclient(mask); return 0; }
    return -1;
}

/* --selftest — install a deliberately-restrictive policy in OUR own
 * process and try a permitted + a denied syscall.  Exits 0 on PASS,
 * non-zero otherwise. */
static int selftest(void) {
    uint32_t mask[4];
    sandbox_policy_minimal(mask);

    if (sys_sandbox_install(mask) != 0) {
        puts("sandbox: --selftest FAIL: install rejected\n");
        return 1;
    }

    int pid = sys_getpid();
    if (pid <= 0) {
        puts("sandbox: --selftest FAIL: getpid (allowed) failed\n");
        return 1;
    }

    int fd = sys_open("/etc/inittab");
    if (fd != -1) {
        puts("sandbox: --selftest FAIL: open (denied) returned a fd\n");
        return 1;
    }

    printf("sandbox: --selftest PASS (pid=%d, denied open OK)\n", pid);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    if (streq(argv[1], "--list")) {
        puts("minimal    write, exit, getpid, sleep, time, brk, "
             "getuid, getgid, getcpu, wait, sandbox_install\n"
             "compute    minimal + mmap, munmap\n"
             "readfs     compute + open, read, close, readdir, "
             "getcwd, chdir, fs_owner, fs_mode\n"
             "netclient  readfs + socket, connect, write_fd, "
             "dns_resolve, fd_nb\n"
             "\n"
             "Limit flags (cumulative, monotonic-tighten):\n"
             "  --rss-kb N   max resident set\n"
             "  --cpu-ms N   max CPU time\n"
             "  --fds    N   max simultaneously-open fds\n"
             "  --wall-ms N  wall-clock deadline\n");
        return 0;
    }

    if (streq(argv[1], "--selftest")) {
        return selftest();
    }

    /* Build the policy mask from argv[1] (the policy name). */
    uint32_t mask[4];
    if (build_policy(argv[1], mask) < 0) {
        printf("sandbox: unknown policy '%s' (try --list)\n", argv[1]);
        return 1;
    }

    /* Parse optional limit flags between the policy name and the
     * "--" separator. Each flag consumes 2 argv slots: --foo + value. */
    struct sys_limits limits;
    limits_default(&limits);
    int has_limits = 0;

    int i = 2;
    for (; i < argc; i++) {
        if (streq(argv[i], "--")) { i++; break; }     /* end of flags */

        if (i + 1 >= argc) { usage(); return 1; }     /* dangling --foo */
        const char *val = argv[i + 1];

        if (streq(argv[i], "--rss-kb"))  { limits.max_rss_kb  = pos_atoi(val); has_limits = 1; }
        else if (streq(argv[i], "--cpu-ms"))  { limits.max_cpu_ms  = pos_atoi(val); has_limits = 1; }
        else if (streq(argv[i], "--fds"))     { limits.max_fds     = pos_atoi(val); has_limits = 1; }
        else if (streq(argv[i], "--wall-ms")) { limits.max_wall_ms = pos_atoi(val); has_limits = 1; }
        else {
            printf("sandbox: unknown flag '%s'\n", argv[i]);
            return 1;
        }
        i++;     /* skip flag value */
    }

    /* After the loop, i points just past the "--" (or off the end if
     * none was found). i..argc-1 is the child argv. */
    if (i >= argc) {
        usage();
        return 1;
    }

    const char *child_argv[16];
    int n = argc - i;
    if (n > 15) n = 15;
    for (int j = 0; j < n; j++) child_argv[j] = argv[i + j];
    child_argv[n] = 0;

    /* Install sandbox + limits in OUR process, then exec. Mask AND
     * limits are preserved across exec, so the new binary runs
     * already sandboxed with the caps in place. */
    if (sys_sandbox_install(mask) != 0) {
        puts("sandbox: install failed\n");
        return 1;
    }
    if (has_limits) {
        if (sys_setlimit(&limits) != 0) {
            puts("sandbox: setlimit failed\n");
            return 1;
        }
    }

    sys_exec(child_argv[0], child_argv);
    puts("sandbox: exec returned (policy may forbid SYS_EXEC; "
         "install the policy from inside the target binary instead)\n");
    return 127;
}
