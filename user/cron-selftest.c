/*
 * cron-selftest — session 77 (scheduled tasks) end-to-end.
 *
 * Creates oneshot + recurring entries, waits for them to fire,
 * checks the on-disk persistence shape, exercises the subscribe
 * path, and verifies invalid-args rejection. Every entry the test
 * creates is cron.delete'd on the way out so a re-run sees a clean
 * /var/cron directory.
 *
 * FS name: "crn-selftest.elf" (16-char cap). Run as `crn-selftest`.
 */
#include "libagent.h"

static int g_fail;
static void expect(int cond, const char *what) {
    if (cond) printf("  PASS  %s\n", what);
    else      { printf("  FAIL  %s\n", what); g_fail++; }
}

#define R_CAP 4096

static int build_entry_id_arg(char *args, int entry_id) {
    int o = 0;
    const char *p = "{\"entry_id\":";
    while (*p) args[o++] = *p++;
    int v = entry_id;
    char tmp[12]; int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
    while (ti) args[o++] = tmp[--ti];
    args[o++] = '}';
    args[o]   = 0;
    return o;
}

static void cron_delete_entry(int entry_id, char *resp, int resp_cap) {
    char args[64];
    build_entry_id_arg(args, entry_id);
    agent_method_call("cron.delete", args, resp, resp_cap);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[cron] selftest \xE2\x80\x94 session 77 + 78\n");

    static char resp[R_CAP];

    /* [1] oneshot delay_sec=2 fires once. */
    agent_method_call("cron.create",
        "{\"kind\":\"oneshot\",\"delay_sec\":2,\"cmd\":\"/echo.elf\","
        "\"args\":[\"hi\"]}",
        resp, sizeof(resp));
    int eid1 = agent_get_int(resp, "entry_id", -1);
    expect(eid1 > 0, "cron.create(oneshot, delay_sec=2) returned entry_id");

    /* [2] Wait ~4 s, then cron.list expects state=expired, run_count=1. */
    sys_sleep_ms(4000);
    agent_method_call("cron.list", "{}", resp, sizeof(resp));
    /* Find the entry by id; verify state and run_count fields appear
     * with the expected values. We use string-search because libagent
     * doesn't do real JSON parsing — and run_count comes after id in
     * the entry summary so it's the first run_count after the id. */
    {
        /* Find substring "\"id\":<eid1>" then check its neighborhood. */
        char hint[32]; int hi = 0;
        const char *p = "\"id\":";
        while (*p) hint[hi++] = *p++;
        int v = eid1;
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) hint[hi++] = tmp[--ti];
        hint[hi] = 0;
        int saw = agent_contains(resp, hint) &&
                  agent_contains(resp, "\"state\":\"expired\"") &&
                  agent_contains(resp, "\"run_count\":1");
        expect(saw, "oneshot fired: state=expired, run_count=1");
    }
    cron_delete_entry(eid1, resp, sizeof(resp));

    /* [3] recurring with max_runs=3, interval_sec=1. */
    agent_method_call("cron.create",
        "{\"kind\":\"recurring\",\"interval_sec\":1,\"max_runs\":3,"
        "\"cmd\":\"/echo.elf\",\"args\":[\"rec\"]}",
        resp, sizeof(resp));
    int eid2 = agent_get_int(resp, "entry_id", -1);
    expect(eid2 > 0, "cron.create(recurring, max_runs=3) returned entry_id");

    /* [4] Wait ~7 s — recurring fires every 1 s, capped at 3 runs.
     *     7 s leaves slack for the first cron tick after create
     *     (up to 1 s) plus the 3 inter-fire gaps. */
    sys_sleep_ms(7000);
    char args2[64];
    build_entry_id_arg(args2, eid2);
    agent_method_call("cron.get", args2, resp, sizeof(resp));
    {
        int rc = agent_get_int(resp, "run_count", -1);
        int hit_expired = agent_contains(resp, "\"state\":\"expired\"");
        expect(rc == 3 && hit_expired,
               "recurring(max_runs=3) finished at run_count=3, state=expired");
    }
    cron_delete_entry(eid2, resp, sizeof(resp));

    /* [5] + [6] Subscribe to a fresh max_runs=2 entry and count
     *           notifications/cron.fired arrivals. */
    {
        int psk = agent_open_persistent();
        if (psk < 0) {
            expect(0, "persistent conn for cron.subscribe");
            expect(0, "exactly 2 notifications/cron.fired received");
        } else {
            agent_method_call("cron.create",
                "{\"kind\":\"recurring\",\"interval_sec\":1,\"max_runs\":2,"
                "\"cmd\":\"/echo.elf\"}",
                resp, sizeof(resp));
            int eid3 = agent_get_int(resp, "entry_id", -1);
            int sub_ok = 0;
            if (eid3 > 0) {
                /* Direct-method dispatch so the response's "ok"
                 * field isn't backslash-escaped inside an MCP
                 * tools/call content block. */
                char sub_req[256];
                int o = 0;
                const char *p = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                                "\"cron.subscribe\",\"params\":"
                                "{\"entry_id\":";
                while (*p) sub_req[o++] = *p++;
                int v = eid3;
                char tmp[12]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
                while (ti) sub_req[o++] = tmp[--ti];
                const char *p2 = "}}";
                while (*p2) sub_req[o++] = *p2++;
                sub_req[o] = 0;
                agent_send_line(psk, sub_req);
                char ack[512];
                agent_recv_line_timed(psk, ack, sizeof(ack), 1000);
                sub_ok = agent_get_bool(ack, "ok", 0);
            }
            expect(eid3 > 0 && sub_ok, "cron.subscribe to recurring entry");

            /* Wait ~6 s for both fires + their notifications. With
             * interval_sec=1, the 1st fire is at <=1 s, 2nd at <=2 s
             * after the first.                                       */
            int fired = 0;
            for (int iter = 0; iter < 60; iter++) {
                sys_sleep_ms(100);
                char line[1024];
                int ln;
                while ((ln = agent_recv_line(psk, line, sizeof(line))) > 0) {
                    if (agent_contains(line, "\"notifications/cron.fired\""))
                        fired++;
                }
                if (fired >= 2) break;
            }
            expect(fired >= 2, "at least 2 notifications/cron.fired received");
            if (eid3 > 0) cron_delete_entry(eid3, resp, sizeof(resp));
            sys_close(psk);
        }
    }

    /* [7] Invalid args (bogus kind) returns -32602 (a JSON-RPC error
     *     envelope, not a result with entry_id). */
    agent_method_call("cron.create",
        "{\"kind\":\"bogus\",\"cmd\":\"/echo.elf\"}",
        resp, sizeof(resp));
    int bad_kind_rejected = (agent_get_int(resp, "entry_id", -1) < 0) &&
                            agent_contains(resp, "\"error\"");
    expect(bad_kind_rejected, "cron.create(kind=bogus) rejected with error");

    /* [8] Persistence: create then verify /var/cron/<id>.json exists by
     *     reading it via the file:// resource path. */
    agent_method_call("cron.create",
        "{\"kind\":\"oneshot\",\"delay_sec\":3600,\"cmd\":\"/echo.elf\"}",
        resp, sizeof(resp));
    int eid4 = agent_get_int(resp, "entry_id", -1);
    if (eid4 > 0) {
        char path[64];
        int o = 0;
        const char *p = "/var/cron/";
        while (*p) path[o++] = *p++;
        int v = eid4;
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) path[o++] = tmp[--ti];
        const char *suf = ".json";
        while (*suf) path[o++] = *suf++;
        path[o] = 0;
        int fd = sys_open(path);
        expect(fd >= 0, "persisted /var/cron/<id>.json is openable");
        if (fd >= 0) sys_close(fd);
        cron_delete_entry(eid4, resp, sizeof(resp));
    } else expect(0, "persistence smoke skipped (create failed)");

    if (g_fail == 0) { printf("[cron] selftest: all checks PASS\n"); return 0; }
    printf("[cron] selftest: %d FAILURE(S)\n", g_fail);
    return 1;
}
