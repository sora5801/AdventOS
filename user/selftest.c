/*
 * selftest — meta-runner for the per-surface selftests.
 *
 * Fork+execs each /<area>-selftest.elf in turn, captures the child's
 * exit code, prints one summary line per test, and exits 0 only if
 * every test passed. The fork+exec boundary is load-bearing: the
 * sandbox + limits selftests install policies on `self` that would
 * otherwise leak into the next test in the sequence — running each
 * in its own pid keeps them isolated.
 *
 * The order matters less than that each one runs in a fresh address
 * space. Within an individual test, see that test's source for the
 * per-assertion order.
 *
 * Run from the in-guest shell:
 *   advent$ selftest
 *
 * Output is grep-friendly (the per-test "[area] selftest: ..." lines
 * come from the child's stdout; the meta-runner adds one summary
 * line per test) so a host-side script can shell in and parse it.
 */
#include "libuser.h"

struct test {
    const char *label;       /* short name for the summary line  */
    const char *path;        /* /<name>.elf the FS exposes       */
};

static const struct test g_tests[] = {
    { "sandbox-selftest",   "/sbx-selftest.elf" },
    { "limits-selftest",    "/lim-selftest.elf" },
    { "kv-selftest",        "/kv-selftest.elf"  },
    { "jobs-selftest",      "/job-selftest.elf" },
    { "subscribe-selftest", "/sub-selftest.elf" },
    { "cron-selftest",      "/crn-selftest.elf" },
};
#define N_TESTS  (int)(sizeof(g_tests) / sizeof(g_tests[0]))

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int total_pass = 0;
    int total_fail = 0;

    for (int i = 0; i < N_TESTS; i++) {
        printf("\n=== %s ===\n", g_tests[i].label);
        /* Inter-test settle. The job table + per-conn subscriber
         * sets in agentd take a beat to drain after the previous
         * child closes its conn; without this pause, cron tests
         * see stale "still running" sleeps from the jobs test
         * and the slot allocator gives them no room. 1500 ms is
         * roughly the SIGKILL latency on a chunked /sleep.elf
         * plus a generous reap cushion. */
        if (i > 0) sys_sleep_ms(1500);
        int pid = sys_fork();
        if (pid < 0) {
            printf("[selftest] %s: fork failed\n", g_tests[i].label);
            total_fail++;
            continue;
        }
        if (pid == 0) {
            const char *argv2[2] = { g_tests[i].path, 0 };
            sys_exec(g_tests[i].path, argv2);
            puts("selftest: exec failed: ");
            puts(g_tests[i].path);
            puts("\n");
            sys_exit(127);
        }
        int code = 0;
        int reaped = sys_wait(&code);
        (void)reaped;
        /* libc's printf doesn't support %-20s width, so pad manually
         * to keep the summary readable.                                 */
        char padded[24];
        int li = 0;
        while (g_tests[i].label[li] && li < 23) {
            padded[li] = g_tests[i].label[li]; li++;
        }
        while (li < 20) padded[li++] = ' ';
        padded[li] = 0;
        if (code == 0) {
            printf("[selftest] %s: PASS\n", padded);
            total_pass++;
        } else {
            printf("[selftest] %s: FAIL (exit=%d)\n",
                   padded, code);
            total_fail++;
        }
    }

    printf("\n[selftest] summary: %d PASS / %d FAIL / %d total\n",
           total_pass, total_fail, N_TESTS);
    return total_fail == 0 ? 0 : 1;
}
