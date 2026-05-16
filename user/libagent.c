/*
 * libagent — see libagent.h for the public surface.
 */
#include "libagent.h"

static int la_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

/* Session 79 — last-response capture. Updated by agent_call (which
 * underlies every one-shot helper). Selftests read it via
 * agent_last_resp() in their FAIL handler. */
static char g_last_resp[1024];

static void capture_last_resp(const char *resp, int n) {
    if (!resp || n <= 0) { g_last_resp[0] = 0; return; }
    int cap = (int)sizeof(g_last_resp) - 1;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) g_last_resp[i] = resp[i];
    g_last_resp[n] = 0;
}

const char *agent_last_resp(void) { return g_last_resp; }

static int connect_localhost_7000(void) {
    int sk = sys_socket();
    if (sk < 0) return -1;
    unsigned char ip[4] = { 127, 0, 0, 1 };
    if (sys_connect(sk, ip, AGENTD_PORT) < 0) {
        sys_close(sk);
        return -1;
    }
    return sk;
}

int agent_send_line(int fd, const char *line) {
    int n = la_strlen(line);
    int sent = 0;
    while (sent < n) {
        int w = sys_write(fd, line + sent, n - sent);
        if (w <= 0) return -1;
        sent += w;
    }
    if (sys_write(fd, "\n", 1) != 1) return -1;
    return 0;
}

int agent_recv_line(int fd, char *buf, int cap) {
    int n = 0;
    while (n < cap - 1) {
        char c;
        int r = sys_read(fd, &c, 1);
        if (r < 0) return -1;          /* would-block / error */
        if (r == 0) return 0;          /* EOF */
        if (c == '\n') { buf[n] = 0; return n; }
        buf[n++] = c;
    }
    buf[n] = 0;
    return -1;                          /* overflow */
}

int agent_recv_line_timed(int fd, char *buf, int cap, int timeout_ms) {
    /* Session 83: zero buf at entry so callers using the substring
     * helpers (agent_get_bool / agent_contains / etc.) on a timed-out
     * or EOF'd read never read uninitialized stack bytes. The old
     * shape only null-terminated on success or overflow; the EOF path
     * (r==0) and the timeout path (r<0 + waited>=timeout_ms) both
     * left whatever the caller had on the stack visible to the helpers.
     * In cron-selftest [5+6], the subscribe ACK timed out (agentd
     * busy with cron-fire spawn+persist), and agent_get_bool(ack,"ok")
     * sometimes read true from leftover stack — flipping sub_ok
     * randomly. Defensive zero closes the read-side; the matching
     * client-side bump of the timeout itself lives in the caller. */
    if (buf && cap > 0) buf[0] = 0;
    int n = 0;
    int waited = 0;
    while (n < cap - 1) {
        char c;
        int r = sys_read(fd, &c, 1);
        if (r > 0) {
            if (c == '\n') { buf[n] = 0; return n; }
            buf[n++] = c;
            continue;
        }
        if (r == 0) { buf[n] = 0; return 0; }    /* EOF */
        /* r < 0 — would-block. Sleep a bit and retry. */
        if (waited >= timeout_ms) { buf[n] = 0; return -1; }
        sys_sleep_ms(10);
        waited += 10;
    }
    buf[n] = 0;
    return -1;
}

int agent_call(const char *json_request, char *resp_buf, int resp_cap) {
    /* Session 83 — zero the caller's resp_buf BEFORE the network call.
     * Without this, any path that returns without successfully filling
     * resp_buf (connect failed, send failed, EOF from agentd, etc.)
     * leaves the PREVIOUS call's response bytes visible to the caller.
     *
     * The concrete bug this fixes: jobs-selftest test [8] cleanup loop
     * reuses a single `resp` buffer across 8 sequential cancels. If the
     * 4th cancel's response was {"killed":true} and the 5th cancel hits
     * EOF (agentd MAX_CONN saturated, conn refused mid-burst), the test
     * called agent_get_bool(resp,"killed",0) and saw `true` from the
     * stale 4th response — concluding the 5th sleeper was killed when
     * in fact agentd never even saw the request. Seven of eight cancels
     * silently leaked their sleepers, cron-selftest's job table stayed
     * full, the whole downstream cron path cascade-failed.
     *
     * Pairs with the MAX_CONN=16 bump in agentd: the bump prevents
     * EOF-on-burst in the first place; this clear prevents the silent
     * false-positive when EOF DOES happen (e.g., agentd crashed, the
     * client is on a real network with packet loss, etc.). Defense in
     * depth — neither alone catches the entire failure space. */
    if (resp_buf && resp_cap > 0) resp_buf[0] = 0;

    int sk = connect_localhost_7000();
    if (sk < 0) { capture_last_resp("(connect failed)", 16); return -1; }
    if (agent_send_line(sk, json_request) < 0) {
        sys_close(sk);
        capture_last_resp("(send failed)", 13);
        return -1;
    }
    int n = agent_recv_line(sk, resp_buf, resp_cap);
    sys_close(sk);
    /* On EOF (n == 0) agent_recv_line may have left bytes from a
     * previous tick in the buffer up to the partial read — make sure
     * we replace them with an empty C-string so caller's substring
     * helpers find nothing. */
    if (n <= 0 && resp_buf && resp_cap > 0) resp_buf[0] = 0;
    capture_last_resp(resp_buf, n);
    return n;
}

int agent_tool_call(const char *tool, const char *args_json,
                    char *resp_buf, int resp_cap) {
    /* Build the envelope into a small stack buffer. Keep it under
     * 2 KiB; tools/call inputs are typically tiny.                 */
    char req[2048];
    int o = 0;
    const char *p = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                    "\"params\":{\"name\":\"";
    while (*p && o < (int)sizeof(req) - 1) req[o++] = *p++;
    while (*tool && o < (int)sizeof(req) - 1) req[o++] = *tool++;
    const char *p2 = "\",\"arguments\":";
    while (*p2 && o < (int)sizeof(req) - 1) req[o++] = *p2++;
    const char *aj = args_json ? args_json : "{}";
    while (*aj && o < (int)sizeof(req) - 1) req[o++] = *aj++;
    if (o + 2 >= (int)sizeof(req)) return -1;
    req[o++] = '}';
    req[o++] = '}';
    req[o]   = 0;
    return agent_call(req, resp_buf, resp_cap);
}

int agent_method_call(const char *method, const char *args_json,
                      char *resp_buf, int resp_cap) {
    char req[2048];
    int o = 0;
    const char *p = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"";
    while (*p && o < (int)sizeof(req) - 1) req[o++] = *p++;
    while (*method && o < (int)sizeof(req) - 1) req[o++] = *method++;
    const char *p2 = "\",\"params\":";
    while (*p2 && o < (int)sizeof(req) - 1) req[o++] = *p2++;
    const char *aj = args_json ? args_json : "{}";
    while (*aj && o < (int)sizeof(req) - 1) req[o++] = *aj++;
    if (o + 1 >= (int)sizeof(req)) return -1;
    req[o++] = '}';
    req[o]   = 0;
    return agent_call(req, resp_buf, resp_cap);
}

int agent_open_persistent(void) {
    int sk = connect_localhost_7000();
    if (sk < 0) return -1;
    sys_fd_nb(sk, 1);
    return sk;
}

/* Substring search for "<key>":  Returns ptr just past the colon. */
const char *agent_find_field(const char *resp, const char *key) {
    if (!resp || !key) return 0;
    int kl = la_strlen(key);
    if (kl <= 0) return 0;
    /* Match the pattern `"<key>":` — leading quote, key bytes, quote,
     * colon. Optional whitespace after the colon is also skipped. */
    for (int i = 0; resp[i]; i++) {
        if (resp[i] != '"') continue;
        int j;
        for (j = 0; j < kl; j++) {
            if (resp[i + 1 + j] != key[j]) break;
        }
        if (j != kl) continue;
        if (resp[i + 1 + kl] != '"') continue;
        if (resp[i + 2 + kl] != ':') continue;
        const char *p = resp + i + 3 + kl;
        while (*p == ' ' || *p == '\t') p++;
        return p;
    }
    return 0;
}

int agent_get_int(const char *resp, const char *key, int fallback) {
    const char *p = agent_find_field(resp, key);
    if (!p) return fallback;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return fallback;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return neg ? -v : v;
}

int agent_get_bool(const char *resp, const char *key, int fallback) {
    const char *p = agent_find_field(resp, key);
    if (!p) return fallback;
    if (p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') return 1;
    if (p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e')
        return 0;
    return fallback;
}

int agent_contains(const char *resp, const char *needle) {
    if (!resp || !needle) return 0;
    int nl = la_strlen(needle);
    if (nl <= 0) return 1;
    for (int i = 0; resp[i]; i++) {
        int j;
        for (j = 0; j < nl; j++) {
            if (resp[i + j] != needle[j]) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

/* Session 79 — agent_test_reset.
 *
 * Sweeps four classes of artifacts from agentd before a selftest's
 * own work begins:
 *
 *   1. Every entry in shell.job.list — cancel + delete each, retrying
 *      `removed:false` slots (jobs that haven't yet hit JS_EXIT after
 *      cancel) up to a ~3 s budget.
 *   2. Every entry in cron.list — cron.delete each (any kind, any
 *      state). Tests own /var/cron at this point; if there's anything
 *      in there, it's stale.
 *   3. Every key under kv://<ns_prefix>/ — best-effort kv.del on each.
 *      No-op if the namespace doesn't exist yet.
 *
 * The reset uses agent_method_call (direct dispatch) so we can parse
 * the response with the substring helpers without dealing with the
 * MCP tools/call escape.
 *
 * Buffers are kept small (4 KiB) since list responses for our scale
 * fit easily. Returns the total artifact count removed; the caller
 * may print it for diagnostics but no test asserts on it. */

static char g_reset_resp[4096];

/* Extract the integer immediately after the first occurrence of
 * "<key>": in `resp`, returning fallback on miss. We can't use
 * agent_get_int because we want the SECOND/third/... occurrence
 * when iterating: cheat by skipping past the previous one. */
static int find_next_int(const char *resp, const char **cursor,
                         const char *key, int fallback) {
    if (!cursor || !*cursor) return fallback;
    int kl = la_strlen(key);
    if (kl <= 0) return fallback;
    const char *s = *cursor;
    for (; *s; s++) {
        if (*s != '"') continue;
        int j;
        for (j = 0; j < kl; j++) if (s[1 + j] != key[j]) break;
        if (j != kl) continue;
        if (s[1 + kl] != '"' || s[2 + kl] != ':') continue;
        const char *p = s + 3 + kl;
        while (*p == ' ' || *p == '\t') p++;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        if (*p < '0' || *p > '9') continue;
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        *cursor = p;
        return neg ? -v : v;
    }
    *cursor = s;
    return fallback;
}

/* Important pattern note (session 79 bug fix): the list response
 * and each subsequent delete response share `g_reset_resp` —
 * a delete call OVERWRITES the buffer the previous list iteration
 * was scanning. Both helpers below FIRST extract every id into a
 * stack-local array, THEN call delete for each. Without this split
 * the cursor walks into freshly-written response bytes and the
 * outer JSON-RPC `"id":1` of each delete response triggers an
 * infinite delete(1) loop. */

static int reset_jobs(void) {
    int removed = 0;
    /* Session 82 followup: this used to SIGKILL every RUN-state job
     * in agentd's table, but that's actively dangerous when the
     * caller is itself running under agentd (e.g. the meta-runner
     * driven via shell.run) — cancelling job_id=0 would kill the
     * caller's own ancestor. There's no syscall that lets a child
     * task introspect its ancestor's agentd-job-id, so we can't
     * reliably skip "self" entries either.
     *
     * Now that signal_send actually delivers SIGKILL across the
     * full TASK_MAX slot range (also a session-82 followup), each
     * test's own per-test cleanup loop reliably reaps its own
     * sleeper children — no cross-test leak. reset_jobs's role
     * shrinks to "delete already-exited zombie slots so the table
     * stays clean for the next test"; the CANCEL path is gone.
     *
     * Net: tests that fork-and-cancel within themselves still work
     * exactly as before; tests that crash-leak a running job will
     * leave it visible to subsequent tests via the state-summary
     * print (which is the right signal — the leak is the bug). */
    agent_method_call("shell.job.list", "{}",
                      g_reset_resp, sizeof(g_reset_resp));
    if (agent_contains(g_reset_resp, "\"jobs\":[]")) return 0;

    int jids[16]; int n_jids = 0;
    const char *cursor = g_reset_resp;
    while (n_jids < 16) {
        int jid = find_next_int(g_reset_resp, &cursor, "job_id", -1);
        if (jid < 0) break;
        jids[n_jids++] = jid;
    }
    for (int k = 0; k < n_jids; k++) {
        char args[64];
        int o = 0;
        const char *p = "{\"job_id\":";
        while (*p) args[o++] = *p++;
        int v = jids[k];
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) args[o++] = tmp[--ti];
        args[o++] = '}'; args[o] = 0;
        /* No cancel — only attempt delete. delete returns
         * removed:false for RUN-state slots (safe no-op). */
        agent_method_call("shell.job.delete", args,
                          g_reset_resp, sizeof(g_reset_resp));
        if (agent_contains(g_reset_resp, "\"removed\":true")) {
            removed++;
        }
    }
    return removed;
}

static int reset_crons(void) {
    int removed = 0;
    agent_method_call("cron.list", "{}",
                      g_reset_resp, sizeof(g_reset_resp));
    /* The outer JSON-RPC envelope ALSO has "id":<rpc_id> — that's
     * NOT a cron entry id; skip it by starting the scan past
     * "result". Otherwise reset_crons would issue a bogus
     * cron.delete entry_id=<rpc_id> as its first call. */
    const char *cursor = g_reset_resp;
    while (*cursor && *cursor != 'r') cursor++;     /* skip to "result" */
    if (cursor[0] == 'r' && cursor[1] == 'e' && cursor[2] == 's' &&
        cursor[3] == 'u' && cursor[4] == 'l' && cursor[5] == 't') {
        cursor += 6;
    }
    int eids[32]; int n_eids = 0;
    while (n_eids < 32) {
        int eid = find_next_int(g_reset_resp, &cursor, "id", -1);
        if (eid <= 0) break;
        eids[n_eids++] = eid;
    }
    for (int k = 0; k < n_eids; k++) {
        char args[64];
        int o = 0;
        const char *p = "{\"entry_id\":";
        while (*p) args[o++] = *p++;
        int v = eids[k];
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) args[o++] = tmp[--ti];
        args[o++] = '}'; args[o] = 0;
        agent_method_call("cron.delete", args,
                          g_reset_resp, sizeof(g_reset_resp));
        if (agent_get_bool(g_reset_resp, "ok", 0)) removed++;
    }
    return removed;
}

static int reset_kv_ns(const char *ns) {
    if (!ns || !ns[0]) return 0;
    int removed = 0;
    /* kv.list returns {"keys":[...]}. We don't care about pagination
     * (the namespace has at most FS_MAX_FILES entries). */
    char list_args[80];
    int o = 0;
    const char *p = "{\"namespace\":\"";
    while (*p) list_args[o++] = *p++;
    while (*ns) list_args[o++] = *ns++;
    const char *q = "\",\"prefix\":\"\"}";
    while (*q) list_args[o++] = *q++;
    list_args[o] = 0;
    agent_method_call("kv.list", list_args,
                      g_reset_resp, sizeof(g_reset_resp));
    /* Walk the keys array. Each string starts with `"` and ends
     * before the next `"`. Skip the leading `"keys":[` then iterate. */
    const char *start = g_reset_resp;
    while (*start && *start != '[') start++;
    if (!*start) return 0;
    start++;
    while (*start && *start != ']') {
        while (*start == ' ' || *start == ',' || *start == '\t') start++;
        if (*start != '"') break;
        start++;
        char key[80];
        int ki = 0;
        while (*start && *start != '"' && ki < (int)sizeof(key) - 1) {
            key[ki++] = *start++;
        }
        key[ki] = 0;
        if (*start == '"') start++;
        if (ki > 0) {
            /* Build kv.del args. */
            char dargs[160];
            int o2 = 0;
            const char *p2 = "{\"namespace\":\"";
            while (*p2) dargs[o2++] = *p2++;
            /* Re-walk ns — list_args has the trailing chars, easier
             * to reconstruct from the original `ns` argument. Caller
             * passed `ns` by pointer; we walked past it. Recompute
             * via the parsed `list_args` instead: scan back to find
             * what came between the second `"` and the third. */
            int li = 0;
            while (list_args[li] && list_args[li] != '"') li++;
            if (list_args[li] == '"') li++;
            while (list_args[li] && list_args[li] != '"') {
                dargs[o2++] = list_args[li++];
            }
            const char *p3 = "\",\"key\":\"";
            while (*p3) dargs[o2++] = *p3++;
            for (int i = 0; i < ki; i++) dargs[o2++] = key[i];
            const char *p4 = "\"}";
            while (*p4) dargs[o2++] = *p4++;
            dargs[o2] = 0;
            agent_method_call("kv.del", dargs,
                              g_reset_resp, sizeof(g_reset_resp));
            if (agent_get_bool(g_reset_resp, "ok", 0)) removed++;
        }
    }
    return removed;
}

int agent_test_reset(const char *ns_prefix) {
    int total = 0;
    total += reset_jobs();
    total += reset_crons();
    if (ns_prefix) total += reset_kv_ns(ns_prefix);
    /* Session 79 — give agentd a beat to process the FINs from the
     * sequence of one-shot conns this function just churned. With
     * MAX_CONN=4 in agentd, four near-simultaneous accept-close
     * cycles can saturate the slot table for one event-loop tick;
     * if the caller immediately opens a fifth conn, add_conn refuses
     * it (closes the fd at accept-time) and the caller's recv sees
     * EOF with no response. 200 ms covers ~20 event-loop ticks,
     * which is plenty for the close_conn() backlog to drain. */
    sys_sleep_ms(200);
    return total;
}
