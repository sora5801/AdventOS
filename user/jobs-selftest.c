/*
 * jobs-selftest — session 74 (background jobs) end-to-end.
 *
 * Each assertion talks to agentd over loopback via libagent. The
 * test is single-conn for the request/response pairs and uses a
 * second persistent conn for the notifications subscription
 * (assertion 7). All conns are sys_close'd before exit so agentd's
 * per-conn subscriber cleanup runs cleanly.
 *
 * Naming note: filename is jobs-selftest.c but the FS-side name
 * (mkfs.py) is shortened to "job-selftest.elf" because FS_NAME_MAX
 * is 16. Run from the in-guest shell as `job-selftest`.
 */
#include "libagent.h"

static int g_fail;
static void expect(int cond, const char *what) {
    if (cond) printf("  PASS  %s\n", what);
    else {
        printf("  FAIL  %s\n", what);
        const char *lr = agent_last_resp();
        if (lr && lr[0]) printf("        resp: %s\n", lr);
        g_fail++;
    }
}

#define R_CAP 4096

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[jobs] selftest \xE2\x80\x94 session 74 + 78\n");

    /* Session 79 hermetic pre-flight. Cancels + deletes any job slot
     * left over from a prior test in this QEMU boot (or this same
     * test's prior run, if invoked back-to-back). Without this the
     * 9-spawn cap test downstream depends on the previous test's
     * cleanup landing in time; with it, every run starts from an
     * empty g_jobs[]. */
    int reset_n = agent_test_reset("");
    if (reset_n > 0) {
        printf("  (pre-flight cleared %d stale agentd artifact(s))\n", reset_n);
    }

    static char resp[R_CAP];

    /* [1] shell.exec_background({cmd:"/echo.elf", args:["hello"]}) */
    int n = agent_method_call("shell.exec_background",
        "{\"cmd\":\"/echo.elf\",\"args\":[\"hello\"]}",
        resp, sizeof(resp));
    int job_id_1 = -1;
    if (n > 0) job_id_1 = agent_get_int(resp, "job_id", -1);
    expect(job_id_1 >= 0, "shell.exec_background(/echo.elf,hello) returned job_id");

    /* [2] shell.job.wait with 2s timeout — expect done=true, exit_code=0 */
    {
        char args[64];
        int o = 0;
        const char *fmt = "{\"job_id\":";
        while (*fmt) args[o++] = *fmt++;
        int v = job_id_1;
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) args[o++] = tmp[--ti];
        const char *fmt2 = ",\"timeout_ms\":2000}";
        while (*fmt2) args[o++] = *fmt2++;
        args[o] = 0;
        n = agent_method_call("shell.job.wait", args, resp, sizeof(resp));
        int done = agent_get_bool(resp, "done", 0);
        int exit_code = agent_get_int(resp, "exit_code", -999);
        expect(done == 1, "shell.job.wait reports done=true within 2 s");
        expect(exit_code == 0, "shell.job.wait reports exit_code=0");
    }

    /* [3] shell.job.read returns stdout containing "hello" */
    {
        char args[64];
        int o = 0;
        const char *fmt = "{\"job_id\":";
        while (*fmt) args[o++] = *fmt++;
        int v = job_id_1;
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) args[o++] = tmp[--ti];
        args[o++] = '}';
        args[o]   = 0;
        n = agent_method_call("shell.job.read", args, resp, sizeof(resp));
        expect(agent_contains(resp, "hello"),
               "shell.job.read returned stdout containing \"hello\"");
    }

    /* Clean up job_id_1 before spawning more. */
    {
        char args[64];
        int o = 0;
        const char *fmt = "{\"job_id\":";
        while (*fmt) args[o++] = *fmt++;
        int v = job_id_1;
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) args[o++] = tmp[--ti];
        args[o++] = '}';
        args[o]   = 0;
        agent_method_call("shell.job.delete", args, resp, sizeof(resp));
    }

    /* [4] Long-running job for the cancel test. /sleep.elf 10 will
     *     park until killed.                                       */
    int job_id_2 = -1;
    n = agent_method_call("shell.exec_background",
        "{\"cmd\":\"/sleep.elf\",\"args\":[\"10\"]}",
        resp, sizeof(resp));
    if (n > 0) job_id_2 = agent_get_int(resp, "job_id", -1);
    expect(job_id_2 >= 0, "shell.exec_background(/sleep.elf,10) returned job_id");

    /* [5] shell.job.cancel returns killed=true */
    if (job_id_2 >= 0) {
        char args[64];
        int o = 0;
        const char *fmt = "{\"job_id\":";
        while (*fmt) args[o++] = *fmt++;
        int v = job_id_2;
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) args[o++] = tmp[--ti];
        args[o++] = '}';
        args[o]   = 0;
        n = agent_method_call("shell.job.cancel", args, resp, sizeof(resp));
        int killed = agent_get_bool(resp, "killed", 0);
        expect(killed == 1, "shell.job.cancel returned killed=true");
    } else expect(0, "shell.job.cancel skipped (no job)");

    /* [6] After cancel, status should report state=\"exited\" within ~500 ms.
     *    Poll a few times — the kernel SIGKILL + reap is async. */
    if (job_id_2 >= 0) {
        char args[64];
        int o = 0;
        const char *fmt = "{\"job_id\":";
        while (*fmt) args[o++] = *fmt++;
        int v = job_id_2;
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) args[o++] = tmp[--ti];
        args[o++] = '}';
        args[o]   = 0;
        int saw_exited = 0;
        for (int iter = 0; iter < 10 && !saw_exited; iter++) {
            sys_sleep_ms(100);
            agent_method_call("shell.job.status", args, resp, sizeof(resp));
            if (agent_contains(resp, "\"state\":\"exited\"")) saw_exited = 1;
        }
        expect(saw_exited == 1,
               "shell.job.status reports state=\"exited\" within ~1 s of cancel");
        agent_method_call("shell.job.delete", args, resp, sizeof(resp));
    } else expect(0, "shell.job.status (post-cancel) skipped");

    /* [7] Subscribe + bounded long-running job; expect
     *     notifications/job.exit on the persistent conn after the
     *     job ends naturally. /echo.elf would exit before the
     *     subscribe call lands; /sleep.elf 1 gives us a full second
     *     to register before the exit notif fires.
     *
     *     We don't assert on notifications/job.output here — sleep
     *     produces no output, and racing a print-and-exit job
     *     against the subscribe registration was the failure mode
     *     of the earlier echo-based version. The KV / resources
     *     subscribe paths in subscribe-selftest already cover the
     *     output side of the notification machinery. */
    {
        int psk = agent_open_persistent();
        if (psk < 0) {
            expect(0, "persistent conn for subscribe");
        } else {
            n = agent_method_call("shell.exec_background",
                "{\"cmd\":\"/sleep.elf\",\"args\":[\"1\"]}",
                resp, sizeof(resp));
            int j3 = agent_get_int(resp, "job_id", -1);

            if (j3 >= 0) {
                char args[256];
                int o = 0;
                const char *fmt = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                                  "\"shell.job.subscribe\",\"params\":"
                                  "{\"job_id\":";
                while (*fmt) args[o++] = *fmt++;
                int v = j3;
                char tmp[12]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
                while (ti) args[o++] = tmp[--ti];
                const char *fmt2 = "}}";
                while (*fmt2) args[o++] = *fmt2++;
                args[o] = 0;
                agent_send_line(psk, args);
                char ack[512];
                agent_recv_line_timed(psk, ack, sizeof(ack), 1000);
            }

            /* Drain notifications for up to ~3 s — sleep 1 finishes
             * in ~1 s, plus the reaper tick. */
            int got_exit = 0;
            for (int iter = 0; iter < 60 && !got_exit; iter++) {
                sys_sleep_ms(50);
                char line[1024];
                int  ln;
                while ((ln = agent_recv_line(psk, line, sizeof(line))) > 0) {
                    if (agent_contains(line, "\"notifications/job.exit\""))
                        got_exit = 1;
                }
            }
            expect(got_exit, "persistent conn received notifications/job.exit");
            sys_close(psk);
            /* clean up the test job */
            if (j3 >= 0) {
                char d[64];
                int o = 0;
                const char *fmt = "{\"job_id\":";
                while (*fmt) d[o++] = *fmt++;
                int v = j3;
                char tmp[12]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
                while (ti) d[o++] = tmp[--ti];
                d[o++] = '}'; d[o] = 0;
                agent_method_call("shell.job.delete", d, resp, sizeof(resp));
            }
        }
    }

    /* [8] Job table cap (JOB_MAX = 8). Burn 8 background sleepers,
     *     then try a 9th — expect a JSON-RPC error (no job_id field).
     *     Slot recycling considers EXIT/REAPED slots, so we deliberately
     *     keep all 8 RUNNING. Clean up by cancelling all afterwards. */
    {
        int spawned[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        int ok = 0;
        for (int i = 0; i < 8; i++) {
            n = agent_method_call("shell.exec_background",
                "{\"cmd\":\"/sleep.elf\",\"args\":[\"30\"]}",
                resp, sizeof(resp));
            spawned[i] = agent_get_int(resp, "job_id", -1);
            if (spawned[i] >= 0) ok++;
        }
        n = agent_method_call("shell.exec_background",
            "{\"cmd\":\"/sleep.elf\",\"args\":[\"30\"]}",
            resp, sizeof(resp));
        int ninth = agent_get_int(resp, "job_id", -1);
        expect(ok == 8 && ninth < 0,
               "9th shell.exec_background rejected when 8 are RUNNING");

        /* Cleanup: cancel + delete each sleeper. SIGKILL on a task
         * parked in sys_sleep_ms takes a few PIT ticks to actually
         * terminate the child; bump the wait so cron-selftest (which
         * runs next in the meta-runner) doesn't see a still-full
         * job table. */
        for (int i = 0; i < 8; i++) {
            if (spawned[i] < 0) continue;
            char d[64];
            int o = 0;
            const char *fmt = "{\"job_id\":";
            while (*fmt) d[o++] = *fmt++;
            int v = spawned[i];
            char tmp[12]; int ti = 0;
            if (v == 0) tmp[ti++] = '0';
            else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
            while (ti) d[o++] = tmp[--ti];
            d[o++] = '}'; d[o] = 0;
            agent_method_call("shell.job.cancel", d, resp, sizeof(resp));
        }
        /* Poll-and-retry delete: SIGKILL on a chunked /sleep.elf
         * lands within 100 ms, but the reaper + drain transition
         * to JS_EXIT can lag a tick or two. Retry deletes until
         * shell.job.list reports an empty table, or budget exhausts. */
        for (int round = 0; round < 30; round++) {
            sys_sleep_ms(100);
            int any_left = 0;
            for (int i = 0; i < 8; i++) {
                if (spawned[i] < 0) continue;
                char d[64];
                int o = 0;
                const char *fmt = "{\"job_id\":";
                while (*fmt) d[o++] = *fmt++;
                int v = spawned[i];
                char tmp[12]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
                while (ti) d[o++] = tmp[--ti];
                d[o++] = '}'; d[o] = 0;
                agent_method_call("shell.job.delete", d, resp, sizeof(resp));
                if (agent_contains(resp, "\"removed\":true") ||
                    agent_get_int(resp, "removed", 0) > 0) {
                    spawned[i] = -1;
                } else {
                    any_left = 1;
                }
            }
            if (!any_left) break;
        }
    }

    if (g_fail == 0) { printf("[jobs] selftest: all checks PASS\n"); return 0; }
    printf("[jobs] selftest: %d FAILURE(S)\n", g_fail);
    return 1;
}
