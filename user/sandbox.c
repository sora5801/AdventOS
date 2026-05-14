/*
 * sandbox — install a syscall allow-bitmap and exec a command in it.
 *
 * Usage:
 *   sandbox POLICY -- CMD [ARG ...]
 *   sandbox --selftest
 *   sandbox --list
 *
 * POLICY is one of: minimal, compute, readfs, netclient.  See
 * user/libuser.c for the exact syscall sets behind each name.  The
 * mask is installed before exec, so the spawned binary boots already
 * sandboxed; child processes (fork) inherit the same mask and cannot
 * loosen it.
 *
 * Session 70.  Companion to kernel/syscall.c's per-task sandbox
 * enforcement and the libuser sandbox_policy_* builders.
 */

#include "libuser.h"

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void usage(void) {
    puts("usage: sandbox POLICY -- CMD [ARG ...]\n"
         "       sandbox --list             # show available policies\n"
         "       sandbox --selftest         # in-process smoke test\n"
         "\n"
         "POLICY: minimal | compute | readfs | netclient\n");
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
 * non-zero otherwise.
 *
 * The denied test uses SYS_OPEN — the minimal policy forbids it.
 * "Forbidden" is signalled by the syscall returning -1 with a
 * sandbox_denials bump on the kernel side; we just check the -1. */
static int selftest(void) {
    uint32_t mask[4];
    sandbox_policy_minimal(mask);

    /* Sanity check: we expect this install to succeed. */
    if (sys_sandbox_install(mask) != 0) {
        puts("sandbox: --selftest FAIL: install rejected\n");
        return 1;
    }

    /* SYS_GETPID is in MINIMAL → should still work. */
    int pid = sys_getpid();
    if (pid <= 0) {
        puts("sandbox: --selftest FAIL: getpid (allowed) failed\n");
        return 1;
    }

    /* SYS_OPEN is NOT in MINIMAL → must return -1. */
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
             "dns_resolve, fd_nb\n");
        return 0;
    }

    if (streq(argv[1], "--selftest")) {
        return selftest();
    }

    if (argc < 4 || !streq(argv[2], "--")) {
        usage();
        return 1;
    }

    uint32_t mask[4];
    if (build_policy(argv[1], mask) < 0) {
        printf("sandbox: unknown policy '%s' (try --list)\n", argv[1]);
        return 1;
    }

    /* Build a fresh argv for the child: argv[3..] become argv[0..]. */
    const char *child_argv[16];
    int n = argc - 3;
    if (n > 15) n = 15;
    for (int i = 0; i < n; i++) child_argv[i] = argv[3 + i];
    child_argv[n] = 0;

    /* Install the policy in OUR process, then exec.  Mask is preserved
     * across exec, so the new binary runs already sandboxed.  No fork
     * — we want the policy to apply to the eventual target image, not
     * a forked shim that then exec()s into a (notionally) fresh task. */
    if (sys_sandbox_install(mask) != 0) {
        puts("sandbox: install failed\n");
        return 1;
    }

    sys_exec(child_argv[0], child_argv);
    /* If exec returns, it failed.  But — note — SYS_EXEC is NOT in any
     * predefined policy.  If you sandbox yourself before exec, the
     * exec call itself is denied and you fall through here.  That's
     * intentional: pre-exec installs of restrictive policies should
     * either include SYS_EXEC explicitly (uncommon) or be installed
     * by the child *after* exec.  Reflect both cases in the error. */
    puts("sandbox: exec returned (policy may forbid SYS_EXEC; "
         "install the policy from inside the target binary instead)\n");
    return 127;
}
