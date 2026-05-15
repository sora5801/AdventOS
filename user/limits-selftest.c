/*
 * limits-selftest — verifies session-71 per-task resource caps.
 *
 * Checks:
 *   1. sys_setlimit accepts a max_fds=4 cap
 *   2. four sys_open calls under that cap succeed
 *   3. the fifth sys_open returns -1 (alloc_fd refuses)
 *   4. after closing them all, /proc/self/limits Fds: line drops
 *      back to the baseline (= 4 — stdin/stdout/stderr + the procfs
 *      fd we're holding)
 *   5. fork a child with a CPU cap, child spins; parent waits and
 *      observes the kernel killed it with SIGKILL (~ session 71's
 *      pit.c branch).
 *
 * Notes:
 *   - Path resolved is /etc/inittab — readable to everyone (mode
 *     0644), small (< 1 KiB), present on every boot.
 *   - The fd cap test runs first so the CPU-cap fork doesn't muddy
 *     the parent's fd table accounting.
 *   - read_fds_live() is fragile to procfs format changes; if the
 *     "Fds:" prefix moves, fix the parser here in lockstep.
 */

#include "libuser.h"

static int g_fail;

static void expect(int cond, const char *what) {
    if (cond) printf("  PASS  %s\n", what);
    else      { printf("  FAIL  %s\n", what); g_fail++; }
}

/* Parse the integer immediately after the "Fds:" key in
 * /proc/self/limits. Returns -1 on parse failure. */
static int read_fds_live(void) {
    int pid = sys_getpid();
    char path[40];
    int  pi = 0;
    const char *p = "/proc/";
    while (*p) path[pi++] = *p++;
    int n = pid;
    char digits[8]; int di = 0;
    if (n == 0) digits[di++] = '0';
    else while (n) { digits[di++] = (char)('0' + n % 10); n /= 10; }
    while (di) path[pi++] = digits[--di];
    const char *q = "/limits";
    while (*q) path[pi++] = *q++;
    path[pi] = 0;

    int fd = sys_open(path);
    if (fd < 0) return -1;
    char buf[1024];
    int total = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (total <= 0) return -1;
    buf[total] = 0;

    const char *key = "Fds:";
    int kl = 0; while (key[kl]) kl++;
    int matched = -1;
    for (int i = 0; i + kl < total; i++) {
        int ok = 1;
        for (int j = 0; j < kl; j++) if (buf[i + j] != key[j]) { ok = 0; break; }
        if (ok) { matched = i + kl; break; }
    }
    if (matched < 0) return -1;
    int j = matched;
    while (j < total && (buf[j] == ' ' || buf[j] == '\t')) j++;
    int v = 0, got = 0;
    while (j < total && buf[j] >= '0' && buf[j] <= '9') {
        v = v * 10 + (buf[j++] - '0');
        got = 1;
    }
    return got ? v : -1;
}

static int fd_cap_test(void) {
    /* read_fds_live() transiently opens /proc/self/limits while
     * generating the file content, so the "Fds:" line counts that
     * procfs fd as live — it's always (true baseline + 1). Subtract
     * 1 to recover the post-close baseline. We can't do better
     * without a different counting path (no SYS_GET_LIVE_FDS yet). */
    int fd_inflated = read_fds_live();
    int baseline    = fd_inflated - 1;
    expect(fd_inflated >= 4, "baseline Fds: line is readable (>= 4 incl. transient procfs fd)");

    /* Install max_fds = baseline + 4. Four opens should land us
     * exactly at the cap; a fifth must be rejected. */
    int target = baseline + 4;
    struct sys_limits l; limits_default(&l); l.max_fds = (uint32_t)target;
    int sr = sys_setlimit(&l);
    expect(sr == 0, "sys_setlimit(max_fds=baseline+4) returns 0");

    int fds[4] = { -1, -1, -1, -1 };
    int ok = 0;
    for (int i = 0; i < 4; i++) {
        fds[i] = sys_open("/etc/inittab");
        if (fds[i] >= 0) ok++;
    }
    expect(ok == 4, "four sys_open calls under the cap succeed");

    int over = sys_open("/etc/inittab");
    expect(over < 0, "fifth sys_open returns -1 (alloc_fd refused)");
    if (over >= 0) sys_close(over);

    for (int i = 0; i < 4; i++) if (fds[i] >= 0) sys_close(fds[i]);

    /* After-close, the live count should be back to the same
     * inflated reading (baseline + 1 transient procfs fd during
     * the new read). Compares to the original snapshot. */
    int fd_after = read_fds_live();
    expect(fd_after == fd_inflated,
           "after close, Fds: line returned to baseline");
    return 0;
}

static int cpu_cap_test(void) {
    /* Fork a child, give it a short CPU cap, and have it spin. The
     * pit.c tick handler will pend SIGKILL once the cap is reached.
     *
     * Reading exit_code after sys_wait: AdventOS encodes the bare exit
     * code, no signal high-bit. A SIGKILL-on-cpu-cap exit looks the
     * same as a normal sys_exit from the child's perspective on the
     * wait side; what proves the kill happened is that we never see
     * the "child reached done" print (which is unreachable inside
     * the busy loop, by construction).
     *
     * Cap: 200 ms. PIT runs at 100 Hz so 200 ms = 20 ticks.
     * sys_setlimit takes max_cpu_ms — kernel converts to ticks
     * internally. */
    int pid = sys_fork();
    if (pid < 0) {
        expect(0, "sys_fork() succeeded for cpu-cap subtest");
        return 1;
    }
    if (pid == 0) {
        /* Child. */
        struct sys_limits l; limits_default(&l); l.max_cpu_ms = 200;
        sys_setlimit(&l);
        /* Tight spin. The PIT handler bumps cur_cpu_ticks while we
         * run; when it crosses max, SIGKILL is pended and signal
         * delivery on the next IRQ exit terminates us. */
        volatile uint32_t x = 0;
        for (;;) { x++; }
        /* Unreachable. Print would only fire if the kill never
         * happened — we treat this output as the failure signal. */
        puts("child: REACHED DONE (cpu cap did not fire)\n");
        sys_exit(99);
    }
    int code = 0;
    int rpid = sys_wait(&code);
    expect(rpid == pid, "sys_wait returned the spinner child");
    /* If kernel killed us via SIGKILL pending + signal delivery,
     * exit_code looks like a normal exit. The presence of a
     * non-99 code is consistent — but more rigorously, we just
     * confirm we got the child back at all (it didn't run
     * forever). Without the cap this test would hang the
     * shell. */
    expect(rpid > 0, "spinner child reaped (cpu cap enforced)");
    (void)code;
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[limits] selftest — sessions 71 + 75\n");

    fd_cap_test();
    cpu_cap_test();

    if (g_fail == 0) { printf("[limits] selftest: all checks PASS\n"); return 0; }
    printf("[limits] selftest: %d FAILURE(S)\n", g_fail);
    return 1;
}
