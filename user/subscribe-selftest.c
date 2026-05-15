/*
 * subscribe-selftest — session 76 (resources/subscribe + kv.watch).
 *
 * Single persistent conn does double duty: it both subscribes AND
 * issues the mutations that should fire notifications on it. The
 * post-mutation drain reads back the response to the kv.put PLUS
 * the notification(s) that the put triggered.
 *
 * All RPC requests on the persistent conn use the direct method
 * shape ({"method":"<name>","params":{...}}) rather than the MCP
 * tools/call envelope — that way `agent_get_int`/`agent_get_bool`
 * can find the response fields without backslash-escape parsing.
 *
 * FS name: "sub-selftest.elf" (16-char cap). Run as `sub-selftest`.
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

/* Drain ALL lines currently buffered on `fd` (non-blocking). Caller
 * inspects what arrived. Returns the number of lines read; their
 * concatenated contents end up in out[0..*out_n].
 * Loops with a small sleep between attempts so the kernel has time
 * to deliver inbound TCP segments. Total budget ~`budget_ms`. */
static int drain_all(int fd, char *out, int cap, int *out_n, int budget_ms) {
    int total = 0;
    int lines = 0;
    int waited = 0;
    while (waited < budget_ms) {
        char line[1024];
        int n;
        while ((n = agent_recv_line(fd, line, sizeof(line))) > 0) {
            for (int i = 0; i < n && total < cap - 1; i++) out[total++] = line[i];
            if (total < cap - 1) out[total++] = '\n';
            lines++;
        }
        sys_sleep_ms(50);
        waited += 50;
    }
    out[total] = 0;
    if (out_n) *out_n = total;
    return lines;
}

#define R_CAP 4096

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[subscribe] selftest \xE2\x80\x94 session 76 + 78\n");

    /* Session 79 — clear any jobs/crons/kv state left by prior test
     * runs. Also clears the "subtest" KV namespace this test uses. */
    int reset_n = agent_test_reset("subtest");
    if (reset_n > 0) {
        printf("  (pre-flight cleared %d stale agentd artifact(s))\n", reset_n);
    }

    static char resp[R_CAP];
    int psk = agent_open_persistent();
    if (psk < 0) { printf("[subscribe] could not open conn\n"); return 1; }

    /* [1] resources/subscribe on a deduplicated URI. /proc/uptime
     *     ticks every PIT; the daemon polls subscribed URIs every
     *     200 ms so we get notifications quickly. */
    const char *sub = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                      "\"resources/subscribe\",\"params\":"
                      "{\"uri\":\"file:///proc/uptime\"}}";
    agent_send_line(psk, sub);
    int n = agent_recv_line_timed(psk, resp, sizeof(resp), 1000);
    int ok1 = (n > 0 && agent_get_bool(resp, "ok", 0) == 1);
    expect(ok1, "resources/subscribe(/proc/uptime) returns ok=true");

    /* [2] /proc/uptime emits whole-second integers, so the FNV-1a
     *     hash only changes when the second flips. Worst-case
     *     latency: 1 second for the second tick, plus 200 ms for
     *     the next poll. Wait 1500 ms to land at least one notif. */
    char drain[4096]; int drain_n = 0;
    drain_all(psk, drain, sizeof(drain), &drain_n, 1500);
    expect(agent_contains(drain, "\"notifications/resources/updated\""),
           "persistent conn received notifications/resources/updated");

    /* [3] resources/unsubscribe removes the subscription cleanly. */
    const char *unsub = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":"
                        "\"resources/unsubscribe\",\"params\":"
                        "{\"uri\":\"file:///proc/uptime\"}}";
    agent_send_line(psk, unsub);
    n = agent_recv_line_timed(psk, resp, sizeof(resp), 1000);
    int ok3 = (n > 0 && agent_get_bool(resp, "ok", 0) == 1);
    expect(ok3, "resources/unsubscribe returns ok=true");

    /* Drain any straggling notifications/resources/updated frames
     * that arrived between our unsubscribe-send and the daemon
     * actually committing the removal. */
    drain_all(psk, drain, sizeof(drain), &drain_n, 100);

    /* [4] kv.watch — direct method, empty prefix matches all keys in ns. */
    const char *kvw = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"kv.watch\","
                      "\"params\":{\"namespace\":\"subtest\","
                      "\"prefix\":\"\"}}";
    agent_send_line(psk, kvw);
    n = agent_recv_line_timed(psk, resp, sizeof(resp), 1000);
    int wid = agent_get_int(resp, "watch_id", -1);
    expect(wid > 0, "kv.watch(subtest, prefix=\"\") returned watch_id");

    /* Tiny pause so the watch is committed before the put. */
    sys_sleep_ms(50);

    /* [5] kv.put on subtest/k1 triggers notifications/kv/changed.
     *     The conn gets BOTH the put response AND the notification;
     *     drain_all collects whatever has arrived after ~200 ms. */
    const char *put1 = "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"kv.put\","
                       "\"params\":{\"namespace\":\"subtest\","
                       "\"key\":\"k1\",\"value\":\"v1\"}}";
    agent_send_line(psk, put1);
    drain_all(psk, drain, sizeof(drain), &drain_n, 200);
    expect(agent_contains(drain, "\"notifications/kv/changed\"") &&
           agent_contains(drain, "\"k1\""),
           "kv.put(subtest,k1) triggered notifications/kv/changed");

    /* [6] kv.put on a DIFFERENT namespace must NOT trigger. */
    const char *put2 = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"kv.put\","
                       "\"params\":{\"namespace\":\"other\","
                       "\"key\":\"k1\",\"value\":\"x\"}}";
    agent_send_line(psk, put2);
    drain_all(psk, drain, sizeof(drain), &drain_n, 200);
    expect(!agent_contains(drain, "\"notifications/kv/changed\""),
           "kv.put on different namespace did NOT trigger notification");

    /* [7] kv.unwatch + put — no notification. */
    {
        char unw[128];
        int o = 0;
        const char *q = "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":"
                        "\"kv.unwatch\",\"params\":{\"watch_id\":";
        while (*q) unw[o++] = *q++;
        int v = wid;
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) unw[o++] = tmp[--ti];
        const char *q2 = "}}";
        while (*q2) unw[o++] = *q2++;
        unw[o] = 0;
        agent_send_line(psk, unw);
        agent_recv_line_timed(psk, resp, sizeof(resp), 1000);
    }
    drain_all(psk, drain, sizeof(drain), &drain_n, 100);

    const char *put3 = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"kv.put\","
                       "\"params\":{\"namespace\":\"subtest\","
                       "\"key\":\"k2\",\"value\":\"v2\"}}";
    agent_send_line(psk, put3);
    drain_all(psk, drain, sizeof(drain), &drain_n, 200);
    expect(!agent_contains(drain, "\"notifications/kv/changed\""),
           "kv.unwatch + kv.put produces no notification");

    /* [8] kv.watch(prefix=abc) + kv.put(key=xyz) — no notification. */
    const char *wa = "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"kv.watch\","
                     "\"params\":{\"namespace\":\"subtest\","
                     "\"prefix\":\"abc\"}}";
    agent_send_line(psk, wa);
    n = agent_recv_line_timed(psk, resp, sizeof(resp), 1000);
    int wid2 = agent_get_int(resp, "watch_id", -1);
    sys_sleep_ms(50);
    const char *put4 = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"kv.put\","
                       "\"params\":{\"namespace\":\"subtest\","
                       "\"key\":\"xyz\",\"value\":\"z\"}}";
    agent_send_line(psk, put4);
    drain_all(psk, drain, sizeof(drain), &drain_n, 200);
    expect(wid2 > 0 &&
           !agent_contains(drain, "\"notifications/kv/changed\""),
           "kv.watch(prefix=abc) does not fire on key=xyz");

    /* Hygiene: clean up the keys we wrote in the subtest namespace.
     * Use direct method for these too. */
    agent_method_call("kv.del",
        "{\"namespace\":\"subtest\",\"key\":\"k1\"}", resp, sizeof(resp));
    agent_method_call("kv.del",
        "{\"namespace\":\"subtest\",\"key\":\"k2\"}", resp, sizeof(resp));
    agent_method_call("kv.del",
        "{\"namespace\":\"other\",\"key\":\"k1\"}", resp, sizeof(resp));

    sys_close(psk);

    if (g_fail == 0) { printf("[subscribe] selftest: all checks PASS\n"); return 0; }
    printf("[subscribe] selftest: %d FAILURE(S)\n", g_fail);
    return 1;
}
