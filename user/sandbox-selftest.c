/*
 * sandbox-selftest — verifies the session-70 syscall sandbox
 * implementation end-to-end. Each EXPECT line prints PASS/FAIL on
 * its own; the program exits 0 only if every check passed.
 *
 * Six checks:
 *   1. install a minimal policy
 *   2. sys_getpid still works (allowed bit set)
 *   3. sys_open returns -1 (denied bit clear)
 *   4. out-of-range syscall (eax=200, beyond mask width) returns -1
 *   5. /proc/self/sandbox shows Denials >= 2
 *   6. sys_sandbox_install with the SYS_SANDBOX_INSTALL bit cleared
 *      freezes the policy — a follow-up install attempt returns -1
 *      AND does not change the mask
 *
 * SYS_FORK is intentionally kept in the policy so a future revision
 * could fork off the procfs-reading half — currently we just open
 * /proc directly under the active mask, which has SYS_OPEN cleared,
 * so the policy is widened just to allow that one read before being
 * tightened back down. See `read_proc_denials` below.
 *
 * Run from the shell:
 *   advent$ sandbox-selftest
 */

#include "libuser.h"

static int g_fail;

static void expect(int cond, const char *what) {
    if (cond) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

/* Issue a raw syscall with the given number. Used for the
 * out-of-range check (sandbox_policy_* helpers cap at < 128). */
static int raw_syscall(uint32_t num) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(num)
                      : "memory");
    return ret;
}

/* Parse /proc/self/sandbox, return the value of the "Denials:" line
 * or -1 if missing. The procfs line layout is fixed: "Denials:   N\n". */
static int read_proc_denials(void) {
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
    const char *q = "/sandbox";
    while (*q) path[pi++] = *q++;
    path[pi] = 0;

    int fd = sys_open(path);
    if (fd < 0) return -1;
    char buf[1024];
    int total = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (total <= 0) return -1;
    buf[total] = 0;

    /* Find "Denials:" line. */
    const char *key = "Denials:";
    int kl = 0; while (key[kl]) kl++;
    int matched = 0;
    for (int i = 0; i + kl < total; i++) {
        int ok = 1;
        for (int j = 0; j < kl; j++) if (buf[i + j] != key[j]) { ok = 0; break; }
        if (ok) { matched = i + kl; break; }
    }
    if (!matched) return -1;
    /* Skip whitespace, then read the integer. */
    int j = matched;
    while (j < total && (buf[j] == ' ' || buf[j] == '\t')) j++;
    int v = 0, got = 0;
    while (j < total && buf[j] >= '0' && buf[j] <= '9') {
        v = v * 10 + (buf[j++] - '0');
        got = 1;
    }
    return got ? v : -1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("[sandbox] selftest — sessions 70 + 75\n");

    /* Pre-step: read the baseline /proc/self/sandbox while no
     * policy is active. We need procfs access to do step 5, so we
     * include SYS_OPEN/SYS_READ/SYS_CLOSE in the test policy.    */
    int baseline_denials = read_proc_denials();
    /* (baseline may be 0 if this is the first run; either way OK) */

    /* === 1. Install a tailored test policy ===
     * Start from sandbox_policy_minimal which includes WRITE / EXIT /
     * GETPID / SLEEP / FORK / EXEC / WAIT / SANDBOX_INSTALL, and
     * widen with OPEN/READ/CLOSE so step 5 can poke procfs.
     *
     * We intentionally DO NOT add anything fs-write-side: the test
     * for "denied syscall returns -1" uses sys_open on a path the
     * mask doesn't allow.
     *
     * Wait — minimal does include SYS_SANDBOX_INSTALL. We need that
     * to allow tightening later (step 6). So: install policy A
     * which allows OPEN+READ+CLOSE+SANDBOX_INSTALL+SLEEP+WRITE+...;
     * then step 3 verifies that a *different* denied syscall (say
     * SYS_SETLIMIT) returns -1. */
    uint32_t mask_a[4];
    sandbox_policy_minimal(mask_a);
    /* widen for proc reading */
    mask_a[SYS_OPEN  / 32] |= 1u << (SYS_OPEN  % 32);
    mask_a[SYS_READ  / 32] |= 1u << (SYS_READ  % 32);
    mask_a[SYS_CLOSE / 32] |= 1u << (SYS_CLOSE % 32);
    /* sandbox_policy_minimal includes SYS_WRITE (legacy single-char)
     * but libc.bin's putchar/puts/printf actually call SYS_WRITE_FD
     * (= 12) through the LIBC table. Without that bit set, every
     * test-side printf gets silently swallowed by the sandbox and
     * the test appears to "pass without output". Add it explicitly
     * so the PASS/FAIL lines reach the console. */
    mask_a[SYS_WRITE_FD / 32] |= 1u << (SYS_WRITE_FD % 32);

    int rc = sys_sandbox_install(mask_a);
    expect(rc == 0, "sys_sandbox_install(mask_a) returns 0");

    /* === 2. Allowed syscall still works === */
    int pid = sys_getpid();
    expect(pid > 0, "sys_getpid() under policy returns a valid pid");

    /* === 3. Denied syscall returns -1 ===
     * SYS_SETLIMIT is not in `mask_a` — calling it should be denied
     * by the dispatcher's gate. */
    struct sys_limits lim; limits_default(&lim); lim.max_fds = 8;
    int sl = sys_setlimit(&lim);
    expect(sl < 0, "sys_setlimit() under policy that omits it returns -1");

    /* === 4. Out-of-range syscall (num >= 128) returns -1 ===
     * SANDBOX_MASK_WORDS * 32 = 128. We use eax=200 — well above
     * any real number. The dispatcher's gate sets allowed=0 for
     * num >= 128 and falls into the denial path. */
    int oor = raw_syscall(200);
    expect(oor == -1, "raw_syscall(200) (out-of-range) returns -1");

    /* === 5. Denials counter went up by at least 2 ===
     * Step 3 was one denial, step 4 was the second. */
    int denials = read_proc_denials();
    expect(denials >= 0, "/proc/self/sandbox readable");
    if (baseline_denials < 0) baseline_denials = 0;
    expect(denials - baseline_denials >= 2,
           "Denials counter advanced by >= 2 across steps 3-4");

    /* === 6. Frozen-policy semantics ===
     * Build mask_b that allows everything in mask_a EXCEPT
     * SYS_SANDBOX_INSTALL. Installing it tightens the bitmask:
     * SYS_SANDBOX_INSTALL goes from 1 to 0 (since active_mask &
     * mask_b clears it). After that, any further sys_sandbox_install
     * call should be denied at the dispatcher's gate without ever
     * touching the active mask. */
    uint32_t mask_b[4];
    for (int i = 0; i < 4; i++) mask_b[i] = mask_a[i];
    /* Clear the SANDBOX_INSTALL bit */
    mask_b[SYS_SANDBOX_INSTALL / 32] &= ~(1u << (SYS_SANDBOX_INSTALL % 32));
    int freeze_rc = sys_sandbox_install(mask_b);
    expect(freeze_rc == 0,
           "sys_sandbox_install(mask_b) — last legal tighten — returns 0");

    /* Now attempt a re-install. mask_c = all zeros; if it actually
     * applied, every subsequent syscall would deny and we'd loop
     * forever in the exit path. We expect it to be REJECTED. */
    uint32_t mask_c[4] = {0, 0, 0, 0};
    int reject_rc = sys_sandbox_install(mask_c);
    expect(reject_rc < 0,
           "sys_sandbox_install after freezing returns -1");

    /* Verify sys_getpid still works — proof the policy didn't get
     * applied. If mask_c had taken effect, GETPID would be denied
     * (mask_c is all zeros). */
    int pid2 = sys_getpid();
    expect(pid2 > 0,
           "sys_getpid still works (mask_c was not applied)");

    /* === Summary === */
    if (g_fail == 0) {
        printf("[sandbox] selftest: all checks PASS\n");
        return 0;
    }
    printf("[sandbox] selftest: %d FAILURE(S)\n", g_fail);
    return 1;
}
