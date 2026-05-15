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
 * Session 79: between tests, the meta-runner prints a one-line
 * agentd state summary (job count + cron count) so flake diagnosis
 * doesn't need a separate manual pass. Each agentd-talking test
 * also runs its own agent_test_reset() pre-flight, so this summary
 * should normally report 0/0 — anything else is a leak signal.
 *
 * Run from the in-guest shell:
 *   advent$ selftest
 *
 * Output is grep-friendly (the per-test "[area] selftest: ..." lines
 * come from the child's stdout; the meta-runner adds one summary
 * line per test) so a host-side script can shell in and parse it.
 */
#include "libagent.h"

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

/* Count the occurrences of a `"<key>":` field in `resp`. Used to
 * tally entries in shell.job.list / cron.list responses — both
 * arrays of objects each containing a known field. Cheap and good
 * enough for a diagnostic counter. */
static int count_fields(const char *resp, const char *key) {
    if (!resp || !key) return 0;
    int kl = 0; while (key[kl]) kl++;
    int n = 0;
    for (int i = 0; resp[i]; i++) {
        if (resp[i] != '"') continue;
        int j;
        for (j = 0; j < kl; j++) if (resp[i + 1 + j] != key[j]) break;
        if (j != kl) continue;
        if (resp[i + 1 + kl] != '"') continue;
        if (resp[i + 2 + kl] != ':') continue;
        n++;
    }
    return n;
}

/* Emit a one-line agentd state summary. Should be all-zeros at the
 * start of each test if hermetic pre-flight is working. A non-zero
 * count is a leak signal — either the prior test missed cleanup,
 * agentd has a stale entry, or both. */
static void print_state_summary(const char *label) {
    static char resp[4096];
    agent_method_call("shell.job.list", "{}", resp, sizeof(resp));
    int jobs = count_fields(resp, "job_id");
    agent_method_call("cron.list", "{}", resp, sizeof(resp));
    int crons = count_fields(resp, "id");
    printf("  [state @ %s] jobs=%d crons=%d\n", label, jobs, crons);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int total_pass = 0;
    int total_fail = 0;

    for (int i = 0; i < N_TESTS; i++) {
        printf("\n=== %s ===\n", g_tests[i].label);
        /* Inter-test settle. With session-79's per-test agent_test_reset
         * pre-flight, the cleanup happens INSIDE the next child rather
         * than waiting between forks — 800 ms is enough for the prior
         * child's close_conn EOFs to propagate to agentd, even with
         * MAX_CONN=4 worth of in-flight conn slots. */
        if (i > 0) sys_sleep_ms(800);
        /* State summary BEFORE the test runs. The agentd-talking tests
         * call agent_test_reset() themselves so this should report
         * 0/0 most of the time; non-zero is a leak signal. (Skipped
         * for sandbox/limits/kv since they don't connect to agentd
         * and the summary's own connect would be a no-op at best.) */
        if (i >= 3) print_state_summary(g_tests[i].label);
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
