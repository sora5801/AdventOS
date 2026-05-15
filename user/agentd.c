/*
 * agentd — JSON-RPC 2.0 + MCP tool surface for external agents.
 *
 *   tcp://127.0.0.1:7000   newline-framed JSON-RPC
 *
 * Two coexisting dispatch modes share the same connection:
 *
 *   Legacy direct-method:                   MCP (Model Context Protocol):
 *     {"method":"time"}                       {"method":"initialize",...}
 *     {"method":"shell.exec",                 {"method":"tools/list"}
 *      "params":{"cmd":"ls.elf",...}}         {"method":"tools/call",
 *                                              "params":{"name":"time",
 *                                                        "arguments":{}}}
 *
 * Tool surface: time, getuid, dns_resolve, dhcp_info, dns_cache_stats,
 * fbinfo, smp_stats, shell.exec, shell.exec_sandboxed, kv.* (session 73),
 * shell.exec_background + shell.job.* (session 74). All reachable via
 * either direct method or MCP tools/call.
 *
 * Session 74 architecture:
 *
 *   ┌────────────────────────────────────────────────────────┐
 *   │ Single-threaded event loop on listen_fd + g_conns[] +  │
 *   │ g_jobs[]. Each iteration:                              │
 *   │   1. sys_accept(listen, nb)  — pick up new connection  │
 *   │   2. for each conn: try_dispatch_one_request()         │
 *   │   3. for each RUNNING job: drain_job_pipes()           │
 *   │   4. reap_finished_jobs()    — sys_wait_nb              │
 *   │   5. sys_sleep_ms(10)        — keep idle CPU low       │
 *   │                                                        │
 *   │ All fds are FD_FL_NONBLOCK; the loop never stalls on   │
 *   │ a quiet pipe or a hung client. Background jobs run as  │
 *   │ children of agentd, capturing stdout+stderr into per-  │
 *   │ job 4 KiB ring buffers. Subscribers receive notifica-  │
 *   │ tions/job.output + notifications/job.exit pushed       │
 *   │ asynchronously over the connection.                    │
 *   └────────────────────────────────────────────────────────┘
 *
 * Each tool is one `emit_fn(json_w *, json_v *params)` that writes
 * its `{...}` result body. The direct path wraps it as `{"result": ...}`
 * inside the JSON-RPC envelope; the tools/call path renders the same
 * fragment into a stack-local buffer, then JSON-encodes that as a
 * string inside an MCP `{"content":[{"type":"text","text":"..."}],
 * "isError":false}`.
 *
 * Manifest: /etc/agent.tools.json is read at startup. We extract
 * its `tools` array and cache its re-emitted JSON bytes — tools/list
 * just splices those bytes into the response.
 *
 * Loopback only — uid 0. No auth. Anyone reaching 127.0.0.1:7000
 * already has shell-level privileges via sshd. See docs/65-mcp-server.md
 * + docs/74-streaming-jobs.md.
 */
#include "libuser.h"
#include "../libjson/libjson.h"

#define AGENTD_PORT     7000

/* MCP protocol version we report from `initialize`. Matches the date
 * stamp used by Anthropic's reference clients as of late 2024 — any
 * negotiation logic at the agent end will treat us as compatible. */
#define MCP_PROTO_VER   "2024-11-05"

/* Per-connection state lives in BSS — no malloc, no per-fork churn.
 *
 * REQ_MAX     largest accepted request line
 * RESP_MAX    largest emitted response (must hold the full JSON-RPC
 *             envelope around a tools/call response, whose inner
 *             content is the JSON-escaped serialization of an
 *             emit_fn's output, so we size for ~2x INNER_MAX)
 * SCRATCH_MAX libjson parser arena for the inbound request
 * MANIFEST_MAX cap on the raw file and the re-emitted tools array
 * SHELL_CAP_MAX  per-stream limit on shell.exec capture (sync path) */
#define REQ_MAX         4096
#define RESP_MAX        8192
#define SCRATCH_MAX     8192
#define MANIFEST_MAX    16384   /* session 74 bump from 8192 — adding the
                                 * 9 shell.exec_background / shell.job.*
                                 * schemas pushes the on-disk manifest to
                                 * ~12 KiB. g_tools_arr also grew in step. */
#define SHELL_CAP_MAX   2048

/* Session 74 — multi-conn event-loop state.
 *
 * MAX_CONN limits how many TCP clients can be open at once. 4 is plenty
 * for a loopback-only daemon servicing a small set of agent processes;
 * each conn carries 20 KiB of BSS (req + resp + scratch + bookkeeping).
 *
 * Each conn moves through a tiny state machine:
 *   CST_FREE         slot unused
 *   CST_IDLE         buffering bytes of the next request line in req[]
 *   CST_SEND         resp[] holds a fully-rendered response; we're
 *                    draining it to the socket in SEND_CHUNK pieces
 *   CST_PENDING_WAIT shell.job.wait outstanding; check on each tick.
 *                    Saved request id replays into the response when
 *                    the job exits or the deadline elapses. */
#define MAX_CONN        4

enum conn_state {
    CST_FREE         = 0,
    CST_IDLE         = 1,
    CST_SEND         = 2,
    CST_PENDING_WAIT = 3,
};

struct conn {
    int             fd;             /* socket fd, -1 = unused              */
    int             state;          /* enum conn_state                     */

    char            req[REQ_MAX];
    int             req_n;          /* bytes buffered, awaiting newline    */

    char            resp[RESP_MAX];
    int             resp_n;         /* bytes valid in resp[]               */
    int             resp_sent;      /* bytes already written to socket     */

    char            scratch[SCRATCH_MAX];

    /* Subscription bitmask — bit i set means this conn is subscribed
     * to job slot i. Drains push notifications/job.output to every
     * subscriber. Cleared on conn close.                              */
    uint32_t        sub_mask;

    /* CST_PENDING_WAIT bookkeeping. The wait id buffer captures the
     * client's request id (number or string) so we can replay it into
     * the deferred response. wait_mcp_wrap is 1 when the wait was
     * launched through a tools/call envelope (MCP) so the resolver
     * knows to wrap the body in {"content":[{type,text}],"isError"}.
     */
    int             wait_job;
    uint32_t        wait_deadline_ms;
    int             wait_id_is_num;
    int             wait_id_num;
    char            wait_id_str[64];
    int             wait_id_str_n;
    int             wait_mcp_wrap;
};

static struct conn g_conns[MAX_CONN];

/* The "current conn" pointer used by resp_begin_result / resp_end /
 * tools-call envelope builders to know which buffer to write into.
 * Single-threaded loop, so this is just a function-arg-by-globals
 * trick — set by try_dispatch_one_request before calling dispatch(),
 * cleared after.                                                     */
static struct conn *g_cur;

/* ============================================================
 * Session 74 — background-job table
 * ============================================================
 *
 * Each job slot owns:
 *   - the child pid (after fork)
 *   - the read ends of the child's stdout/stderr pipes (FD_PIPE_R,
 *     marked FD_FL_NONBLOCK so drains never stall)
 *   - two 4 KiB ring buffers carrying the most recent stdout/stderr
 *     bytes. `total` advances monotonically with every appended byte;
 *     `buf` is a circular ring sized JOB_RING_SZ. When total exceeds
 *     JOB_RING_SZ the oldest data is overwritten. Readers see the
 *     window [max(0, total - JOB_RING_SZ) .. total).
 *   - a subscriber bitmask — see struct conn.sub_mask.
 *   - the start tick + cmd label, just for diagnostics.
 *
 * State transitions:
 *   FREE  -> RUN   on shell.exec_background success
 *   RUN   -> EXIT  by reap_finished_jobs() once sys_wait_nb claims it
 *                  AND both stdout / stderr pipes hit EOF
 *   EXIT  -> FREE  by shell.job.delete or implicitly when the table
 *                  fills up (oldest EXIT slot recycled).
 *
 * Lossy by design: if a subscriber falls behind and the ring wraps,
 * the next push notification will simply carry the freshest 4 KiB
 * window. Pull-mode shell.job.read reports `skipped` so the client
 * knows it lost bytes. */
#define JOB_MAX        8
#define JOB_RING_SZ    4096
#define JOB_CMD_MAX    128
#define JOB_POL_MAX    16

enum job_state {
    JS_FREE = 0,
    JS_RUN  = 1,
    JS_EXIT = 2,
};

struct job_ring {
    uint32_t total;                 /* monotonic bytes ever written       */
    char     buf[JOB_RING_SZ];      /* circular window of last JOB_RING_SZ */
};

struct job {
    int             state;          /* enum job_state                     */
    int             pid;
    int             exit_code;      /* meaningful in JS_EXIT              */

    int             out_fd;         /* -1 once closed                     */
    int             err_fd;
    int             out_eof;        /* observed pipe_read returning 0     */
    int             err_eof;
    int             reaped;         /* sys_wait_nb has claimed the pid    */

    struct job_ring out;
    struct job_ring err;

    char            cmd[JOB_CMD_MAX];
    int             cmd_n;
    char            policy[JOB_POL_MAX];  /* "" if no sandbox             */
    int             sandboxed;

    uint32_t        sub_mask;       /* bit i = conn i subscribed          */
    uint32_t        start_tick;     /* sys_time() at spawn                */
};

static struct job g_jobs[JOB_MAX];

/* ============================================================
 * Session 76 — subscription tables
 * ============================================================
 *
 * Two parallel surfaces, both delivering server-initiated
 * `notifications/...` lines to subscribed conns without a request:
 *
 *   resources/subscribe  — polling-based change detection. URI's
 *     content is re-hashed every 20 event-loop ticks (~200 ms).
 *     On hash mismatch, every subscriber for that URI receives a
 *     `notifications/resources/updated {uri}` push. The notification
 *     does NOT carry the new content; clients call `resources/read`
 *     if they want it. MCP-standard.
 *
 *   kv.watch  — event-driven. Every successful `kv.put` / `kv.del`
 *     calls `kv_notify_change(ns, key, op)`, which walks the watch
 *     table and emits `notifications/kv/changed {namespace, key,
 *     op}` to each (namespace, prefix)-matching watcher. AdventOS
 *     extension under `experimental.adventos.kv_watch`.
 *
 * URIs are deduplicated: 4 conns subscribed to /proc/uptime share
 * one `res_uri` entry. The polling loop hashes once per tick per
 * URI, regardless of how many subscribers attach to it.
 */
#define MAX_RES_URIS     16    /* distinct subscribed URIs              */
#define MAX_RES_SUBS     32    /* (conn, uri) pairs                     */
#define MAX_KV_WATCHES   16    /* (conn, namespace, prefix) tuples      */
#define RES_URI_MAX      128   /* per-URI string length                 */
#define KV_WATCH_NS_MAX  32
#define KV_WATCH_PREFIX_MAX 64

struct res_uri {
    int      in_use;
    char     uri[RES_URI_MAX];     /* NUL-terminated                    */
    int      uri_len;              /* strlen(uri)                       */
    uint32_t last_hash;            /* FNV-1a 32 of last-fetched content */
};

struct res_sub {
    int      in_use;
    int      conn_fd;              /* socket fd, not g_conns idx        */
    int      uri_idx;              /* index into g_res_uris             */
};

struct kv_watch {
    int      in_use;
    int      id;                   /* monotonic; kv.unwatch identifier  */
    int      conn_fd;
    char     ns[KV_WATCH_NS_MAX];
    char     prefix[KV_WATCH_PREFIX_MAX]; /* "" = match-all in this ns  */
};

static struct res_uri   g_res_uris  [MAX_RES_URIS];
static struct res_sub   g_res_subs  [MAX_RES_SUBS];
static struct kv_watch  g_kv_watches[MAX_KV_WATCHES];
static int              g_kv_watch_next_id;   /* monotonic, never reused */

/* FNV-1a 32-bit hash. The polling tick uses this to fingerprint a
 * resource's content cheaply — 32-bit collisions occur ~1 in 2^32,
 * acceptable for v1; agents wanting strict semantics can resync via
 * a periodic resources/read. */
static uint32_t fnv1a32(const void *buf, int len) {
    uint32_t h = 0x811C9DC5u;
    const unsigned char *p = (const unsigned char *)buf;
    for (int i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Buffer cap for both the polling tick AND the URI-validation read
 * inside handle_resources_subscribe. 4 KiB covers every procfs
 * file we generate today (gen_sandbox's session-75 ceiling is ~2
 * KiB after the tmp buffer bump) plus typical /etc files. A
 * resource that exceeds this gets its prefix hashed — agents that
 * subscribe to a giant URI are choosing to detect changes in the
 * first 4 KiB only. */
#define RES_POLL_BUF 4096

/* Session 76 forward decls — emit_kv_{put,del} fire kv_notify_change
 * after the write succeeds, but that function lives further down in
 * the file alongside the other notification machinery. */
static void kv_notify_change(const char *ns, const char *key, const char *op);

/* ============================================================ */

/* Manifest cache. Populated once at boot from /etc/agent.tools.json
 * via load_tools_manifest; tools/list splices g_tools_arr into its
 * response. Re-emitted (not raw file bytes) so the JSON is parser-
 * round-tripped — guaranteed well-formed, no embedded whitespace
 * surprises, no comments to handle. */
/* The re-emitted tools array — ~12 KiB now after the session 74
 * shell.exec_background / shell.job.* additions. Sized to comfortably
 * hold the manifest plus the libjson re-emit envelope. */
static char g_tools_arr[16384];
static int  g_tools_arr_len;

/* ============================================================
 * Generic helpers
 * ============================================================ */

static int str_eq(const char *a, int an, const char *b) {
    int bn = 0; while (b[bn]) bn++;
    if (an != bn) return 0;
    for (int i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Recursive walker that re-emits a parsed json_v tree into a writer.
 * Used both for tools/list (cached) and for tools/call argument
 * forwarding where we hand sub-objects to nested emitters. */
static void emit_value(struct json_w *w, const struct json_v *v) {
    if (!v) { json_null(w); return; }
    switch (v->type) {
        case JSON_NULL: json_null(w); break;
        case JSON_BOOL: json_bool(w, (int)v->num); break;
        case JSON_NUM:  json_int(w,  (int)v->num); break;
        case JSON_STR:  json_str_n(w, v->str, v->str_len); break;
        case JSON_ARR:
            json_arr_begin(w);
            for (struct json_v *c = v->child; c; c = c->next) emit_value(w, c);
            json_arr_end(w);
            break;
        case JSON_OBJ:
            json_obj_begin(w);
            /* libjson stores object children as alternating key/value
             * via the `next` pointer: k1 → v1 → k2 → v2 → ... */
            for (struct json_v *k = v->child; k && k->next; k = k->next->next) {
                if (k->type == JSON_STR) {
                    /* p_string NUL-terminates after str_len, so the
                     * arena pointer is a valid C-string for json_key. */
                    json_key(w, k->str);
                    emit_value(w, k->next);
                }
            }
            json_obj_end(w);
            break;
        default: json_null(w); break;
    }
}

/* ============================================================
 * Manifest loader (called once at boot)
 * ============================================================ */

static void load_tools_manifest(void) {
    /* These buffers are only live during this one boot-time call, so
     * stack-allocate them rather than burning BSS that would stay
     * resident forever. Scratch is sized for libjson's DOM: one
     * json_v per value (~28B) plus decoded string bodies. 32 KiB
     * covers the 23-tool manifest with plenty of headroom. Stack
     * budget is 64 KiB (USER_STACK_PAGES=16), so 16+32 KiB fits. */
    char raw[MANIFEST_MAX];
    char scratch[32768];

    int fd = sys_open("/etc/agent.tools.json");
    if (fd < 0) {
        puts("agentd: /etc/agent.tools.json missing — tools/list will be empty\n");
        g_tools_arr[0] = '['; g_tools_arr[1] = ']';
        g_tools_arr_len = 2;
        return;
    }
    int total = 0, n;
    while (total < (int)sizeof(raw) - 1 &&
           (n = sys_read(fd, raw + total, sizeof(raw) - 1 - total)) > 0) {
        total += n;
    }
    sys_close(fd);
    raw[total] = 0;

    struct json_v *root = json_parse(raw, total, scratch, sizeof(scratch));
    if (!root || root->type != JSON_OBJ) {
        puts("agentd: manifest parse failed\n");
        g_tools_arr[0] = '['; g_tools_arr[1] = ']';
        g_tools_arr_len = 2;
        return;
    }
    const struct json_v *tools = json_obj_get(root, "tools");
    if (!tools || tools->type != JSON_ARR) {
        puts("agentd: manifest missing \"tools\" array\n");
        g_tools_arr[0] = '['; g_tools_arr[1] = ']';
        g_tools_arr_len = 2;
        return;
    }
    struct json_w w;
    json_w_init(&w, g_tools_arr, sizeof(g_tools_arr));
    emit_value(&w, tools);
    json_w_finish(&w);
    if (!json_w_ok(&w)) {
        puts("agentd: manifest re-emit overflowed\n");
        g_tools_arr[0] = '['; g_tools_arr[1] = ']';
        g_tools_arr_len = 2;
        return;
    }
    g_tools_arr_len = json_w_len(&w);
    /* manifest loaded — silent */
}

/* ============================================================
 * Response envelope builders
 * ============================================================ */

static void emit_id(struct json_w *w, const struct json_v *id) {
    if (!id || id->type == JSON_NULL)        json_null(w);
    else if (id->type == JSON_NUM)           json_int(w, (int)id->num);
    else if (id->type == JSON_STR)           json_str_n(w, id->str, id->str_len);
    else                                     json_null(w);
}

/* Initialise w over g_cur->resp and write the envelope opening up to
 * "result":, leaving the caller to emit the result value (an
 * object or scalar) and then call resp_end(). g_cur is set by
 * try_dispatch_one_request() before any dispatcher runs, so every
 * handler points at the correct connection's response buffer. */
static void resp_begin_result(struct json_w *w, const struct json_v *id) {
    json_w_init(w, g_cur->resp, sizeof(g_cur->resp));
    json_obj_begin(w);
      json_key(w, "jsonrpc"); json_str(w, "2.0");
      json_key(w, "id");      emit_id(w, id);
      json_key(w, "result");
}

static void resp_end(struct json_w *w) {
    json_obj_end(w);
    /* NUL-terminate at the actual response length. Without this,
     * a downstream strlen() walks into stale bytes left over from
     * a previous request — same buffer reused across this conn's
     * lifetime, never zeroed. */
    json_w_finish(w);
}

static void resp_error(const struct json_v *id, int code, const char *msg,
                       char *out_buf, int out_cap, int *out_n) {
    struct json_w w;
    json_w_init(&w, out_buf, out_cap);
    json_obj_begin(&w);
      json_key(&w, "jsonrpc"); json_str(&w, "2.0");
      json_key(&w, "id");      emit_id(&w, id);
      json_key(&w, "error");
      json_obj_begin(&w);
        json_key(&w, "code");    json_int(&w, code);
        json_key(&w, "message"); json_str(&w, msg);
      json_obj_end(&w);
    json_obj_end(&w);
    json_w_finish(&w);
    *out_n = json_w_len(&w);
}


/* ============================================================
 * Per-tool emitters
 * ============================================================
 *
 * Each emit_X_result writes a single result-object body into the
 * supplied writer. Return 0 on success, JSON-RPC error code on
 * failure (caller will discard the partial output and emit an
 * error envelope instead).
 *
 * The signature is shared between the legacy direct dispatch and
 * the MCP tools/call path — same function, different envelope. */

typedef int (*emit_fn)(struct json_w *w, const struct json_v *params);

static int emit_time(struct json_w *w, const struct json_v *params) {
    (void)params;
    json_obj_begin(w);
      json_key(w, "epoch"); json_uint(w, sys_time());
    json_obj_end(w);
    return 0;
}

static int emit_getuid(struct json_w *w, const struct json_v *params) {
    (void)params;
    json_obj_begin(w);
      json_key(w, "uid"); json_int(w, sys_getuid());
      json_key(w, "gid"); json_int(w, sys_getgid());
    json_obj_end(w);
    return 0;
}

static void emit_dotted_quad(struct json_w *w, const unsigned char ip[4]) {
    char dq[16];
    int  o = 0;
    for (int i = 0; i < 4; i++) {
        unsigned int v = ip[i];
        char tmp[4]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti) dq[o++] = tmp[--ti];
        if (i < 3) dq[o++] = '.';
    }
    json_str_n(w, dq, o);
}

static int emit_dns_resolve(struct json_w *w, const struct json_v *params) {
    const struct json_v *hv = json_obj_get(params, "host");
    int hlen = 0;
    const char *hs = json_to_str(hv, &hlen);
    if (!hs || hlen <= 0 || hlen > 250) return -32602;

    unsigned char ip[4];
    if (sys_dns_resolve(hs, ip) < 0) return -32000;

    json_obj_begin(w);
      json_key(w, "ip"); emit_dotted_quad(w, ip);
    json_obj_end(w);
    return 0;
}

static int emit_dhcp_info(struct json_w *w, const struct json_v *params) {
    (void)params;
    struct sys_dhcp_info di;
    sys_dhcp_info(&di);
    json_obj_begin(w);
      json_key(w, "have_lease");      json_bool(w, di.have_lease);
      json_key(w, "ip");              emit_dotted_quad(w, di.ip);
      json_key(w, "netmask");         emit_dotted_quad(w, di.netmask);
      json_key(w, "gateway");         emit_dotted_quad(w, di.gateway);
      json_key(w, "dns_server");      emit_dotted_quad(w, di.dns_server);
      json_key(w, "lease_seconds");   json_uint(w, di.lease_seconds);
      json_key(w, "acquired_epoch");  json_uint(w, di.acquired_epoch);
      json_key(w, "t1_renew_at");     json_uint(w, di.t1_renew_at);
    json_obj_end(w);
    return 0;
}

static int emit_dns_cache_stats(struct json_w *w, const struct json_v *params) {
    (void)params;
    unsigned int out[4] = {0};
    sys_dns_cache_stats(out);
    json_obj_begin(w);
      json_key(w, "lookups");    json_uint(w, out[0]);
      json_key(w, "hits");       json_uint(w, out[1]);
      json_key(w, "misses");     json_uint(w, out[2]);
      json_key(w, "evictions");  json_uint(w, out[3]);
    json_obj_end(w);
    return 0;
}

static int emit_fbinfo(struct json_w *w, const struct json_v *params) {
    (void)params;
    unsigned int info[4] = {0};
    int on = sys_fbinfo(info);
    json_obj_begin(w);
      if (on > 0) {
          json_key(w, "enabled"); json_bool(w, 1);
          json_key(w, "width");   json_uint(w, info[0]);
          json_key(w, "height");  json_uint(w, info[1]);
          json_key(w, "bpp");     json_uint(w, info[2]);
          json_key(w, "pitch");   json_uint(w, info[3]);
      } else {
          json_key(w, "enabled"); json_bool(w, 0);
      }
    json_obj_end(w);
    return 0;
}

static int emit_smp_stats(struct json_w *w, const struct json_v *params) {
    (void)params;
    unsigned int out[8] = {0};
    sys_smp_stats(out);
    json_obj_begin(w);
      json_key(w, "nr_cpus"); json_uint(w, out[0]);
      json_key(w, "ticks");
      json_arr_begin(w);
        unsigned int n = out[0]; if (n > 7) n = 7;
        for (unsigned int i = 0; i < n; i++) json_uint(w, out[1 + i]);
      json_arr_end(w);
    json_obj_end(w);
    return 0;
}

/* --- shell.exec ----------------------------------------------------
 *
 * Fork+exec a command, capture stdout / stderr, wait, return
 * {exit_code, stdout, stderr}. agentd runs as uid 0 so the
 * spawned child has the same privilege; loopback-only access is
 * the perimeter check.
 *
 * Caps stdout / stderr at SHELL_CAP_MAX each (~2 KiB). Long output
 * is truncated silently — agents wanting more should pipe through
 * head/wc/grep first. */

static int drain_fd(int fd, char *buf, int cap) {
    int total = 0;
    int n;
    while (total < cap - 1 &&
           (n = sys_read(fd, buf + total, cap - 1 - total)) > 0) {
        total += n;
    }
    buf[total] = 0;
    return total;
}

/* Session 71 — parsed-out resource caps for the shell.exec_sandboxed
 * path. NULL on the bare shell.exec path. */
struct sb_limits_in {
    int      have;            /* any non-zero field => 1 */
    uint32_t max_rss_kb;
    uint32_t max_cpu_ms;
    uint32_t max_fds;
    uint32_t max_wall_ms;
};

/* Common shell.exec / shell.exec_sandboxed engine.  The sandboxed
 * variant rewrites argv to ["sandbox.elf", policy, "--", cmd, args...]
 * and execs the wrapper instead of the bare command.  Resource limits,
 * if supplied, are installed by the agentd-side fork *in the child*
 * before the wrapper exec — that way the wrapper, sandbox.elf, and
 * the eventual target all share the same caps without burning argv
 * slots on --rss-kb / --cpu-ms / etc. */
static int shell_exec_inner(struct json_w *w,
                            const struct json_v *params,
                            const char *policy            /* NULL = no sandbox */,
                            const struct sb_limits_in *L  /* NULL = no caps */) {
    const struct json_v *cmd_v = json_obj_get(params, "cmd");
    int cmd_len = 0;
    const char *cmd = json_to_str(cmd_v, &cmd_len);
    if (!cmd) return -32602;

    const struct json_v *args_v = json_obj_get(params, "args");
    if (args_v && args_v->type != JSON_ARR) return -32602;
    int n_args = json_arr_len(args_v);
    if (n_args < 0) n_args = 0;
    if (n_args > 12) return -32602;     /* leave headroom for the wrapper prefix */

    /* Build argv. If sandboxed, prepend ["sandbox.elf", policy, "--"]. */
    const char *argv[20];
    const char *exec_path;
    int ai = 0;
    if (policy) {
        exec_path = "sandbox.elf";
        argv[ai++] = "sandbox.elf";
        argv[ai++] = policy;
        argv[ai++] = "--";
    } else {
        exec_path = cmd;
    }
    argv[ai++] = cmd;
    for (int i = 0; i < n_args; i++) {
        const struct json_v *a = json_arr_at(args_v, i);
        int al = 0;
        const char *as = json_to_str(a, &al);
        if (!as) return -32602;
        argv[ai++] = as;
    }
    argv[ai] = 0;

    int out_pp[2], err_pp[2];
    if (sys_pipe(out_pp) < 0) return -32603;
    if (sys_pipe(err_pp) < 0) {
        sys_close(out_pp[0]); sys_close(out_pp[1]);
        return -32603;
    }

    int pid = sys_fork();
    if (pid < 0) {
        sys_close(out_pp[0]); sys_close(out_pp[1]);
        sys_close(err_pp[0]); sys_close(err_pp[1]);
        return -32603;
    }
    if (pid == 0) {
        sys_dup2(out_pp[1], 1);
        sys_dup2(err_pp[1], 2);
        sys_close(out_pp[0]); sys_close(out_pp[1]);
        sys_close(err_pp[0]); sys_close(err_pp[1]);
        /* Session 71: install caps in the child before exec. Caps
         * survive exec, so sandbox.elf and the eventual target both
         * inherit them. We do this only for sandboxed runs — bare
         * shell.exec stays uncapped. */
        if (L && L->have) {
            struct sys_limits sl;
            sl.max_rss_kb  = L->max_rss_kb;
            sl.max_cpu_ms  = L->max_cpu_ms;
            sl.max_fds     = L->max_fds;
            sl.max_wall_ms = L->max_wall_ms;
            sys_setlimit(&sl);
        }
        sys_exec(exec_path, argv);
        sys_write(2, "agentd-exec: cannot exec\n", 25);
        sys_exit(127);
    }
    sys_close(out_pp[1]);
    sys_close(err_pp[1]);

    static char out_buf[SHELL_CAP_MAX];
    static char err_buf[SHELL_CAP_MAX];
    int out_n = drain_fd(out_pp[0], out_buf, sizeof(out_buf));
    int err_n = drain_fd(err_pp[0], err_buf, sizeof(err_buf));
    sys_close(out_pp[0]);
    sys_close(err_pp[0]);

    /* Session 70 + 71: before reaping, peek at /proc/<child>/sandbox
     * AND /proc/<child>/limits. The child is in TASK_STATE_ZOMBIE at
     * this point; procfs still shows zombies so both per-task files
     * are readable. Once sys_wait reaps the task, the state vanishes.
     * Only meaningful for the sandboxed variant — the bare shell.exec
     * has no policy and no caps so both files would be uninformative. */
    static char sb_buf[1024];
    static char lm_buf[512];
    int sb_n = 0;
    int lm_n = 0;
    if (policy) {
        /* Build "/proc/<pid>/" prefix once. */
        char path[40];
        int  pp_len = 0;
        const char *p1 = "/proc/";
        while (*p1) path[pp_len++] = *p1++;
        int n = pid;
        if (n == 0) path[pp_len++] = '0';
        else {
            char tmp[8]; int ti = 0;
            while (n) { tmp[ti++] = (char)('0' + n%10); n /= 10; }
            while (ti) path[pp_len++] = tmp[--ti];
        }
        path[pp_len++] = '/';

        /* /proc/<pid>/sandbox */
        {
            int t = pp_len;
            const char *p2 = "sandbox";
            while (*p2) path[t++] = *p2++;
            path[t] = 0;
            int sfd = sys_open(path);
            if (sfd >= 0) {
                sb_n = sys_read(sfd, sb_buf, sizeof(sb_buf) - 1);
                sys_close(sfd);
                if (sb_n < 0) sb_n = 0;
                sb_buf[sb_n] = 0;
            }
        }

        /* /proc/<pid>/limits */
        {
            int t = pp_len;
            const char *p2 = "limits";
            while (*p2) path[t++] = *p2++;
            path[t] = 0;
            int sfd = sys_open(path);
            if (sfd >= 0) {
                lm_n = sys_read(sfd, lm_buf, sizeof(lm_buf) - 1);
                sys_close(sfd);
                if (lm_n < 0) lm_n = 0;
                lm_buf[lm_n] = 0;
            }
        }
    }

    int code = 0;
    sys_wait(&code);

    int pol_len = 0;
    if (policy) {
        while (policy[pol_len]) pol_len++;
    }

    json_obj_begin(w);
      json_key(w, "exit_code"); json_int(w, code);
      json_key(w, "stdout");    json_str_n(w, out_buf, out_n);
      json_key(w, "stderr");    json_str_n(w, err_buf, err_n);
      if (policy) {
          json_key(w, "policy");       json_str_n(w, policy, pol_len);
          json_key(w, "child_pid");    json_int  (w, pid);
          json_key(w, "sandbox_log");  json_str_n(w, sb_buf, sb_n);
          json_key(w, "limits_state"); json_str_n(w, lm_buf, lm_n);
      }
    json_obj_end(w);
    return 0;
}

static int emit_shell_exec(struct json_w *w, const struct json_v *params) {
    return shell_exec_inner(w, params, /*policy=*/0, /*limits=*/0);
}

/* Session 70: exec a command under a syscall sandbox policy. The
 * `policy` field selects from the libuser policy templates that
 * sandbox.elf knows: minimal | compute | readfs | netclient. The
 * spawned process and any of its children all run with that mask.
 * The wrapper itself burns 4 KiB of address space in the child;
 * everything else is the same as shell.exec. */
static int emit_shell_exec_sandboxed(struct json_w *w,
                                     const struct json_v *params) {
    const struct json_v *p_v = json_obj_get(params, "policy");
    int p_len = 0;
    const char *policy = json_to_str(p_v, &p_len);
    if (!policy) return -32602;

    /* Validate against the known set so a bad name fails fast inside
     * the daemon rather than emitting "sandbox: unknown policy ..."
     * to stderr from the child. */
    if (!(p_len ==  7 && policy[0]=='m' && policy[1]=='i' && policy[2]=='n' &&
                        policy[3]=='i' && policy[4]=='m' && policy[5]=='a' &&
                        policy[6]=='l')
     && !(p_len ==  7 && policy[0]=='c' && policy[1]=='o' && policy[2]=='m' &&
                        policy[3]=='p' && policy[4]=='u' && policy[5]=='t' &&
                        policy[6]=='e')
     && !(p_len ==  6 && policy[0]=='r' && policy[1]=='e' && policy[2]=='a' &&
                        policy[3]=='d' && policy[4]=='f' && policy[5]=='s')
     && !(p_len ==  9 && policy[0]=='n' && policy[1]=='e' && policy[2]=='t' &&
                        policy[3]=='c' && policy[4]=='l' && policy[5]=='i' &&
                        policy[6]=='e' && policy[7]=='n' && policy[8]=='t')) {
        return -32602;
    }

    /* Make a NUL-terminated copy on the stack so we can pass it as
     * argv[1] to sandbox.elf without depending on json_to_str's
     * length-prefixed buffer surviving. */
    char policy_buf[16];
    for (int i = 0; i < p_len && i < (int)sizeof(policy_buf) - 1; i++) {
        policy_buf[i] = policy[i];
    }
    policy_buf[p_len] = 0;

    /* Session 71 — optional `limits` object. Any subset of the four
     * fields may be present; missing fields default to 0 = "no cap".
     * Bad shapes (limits present but not an object, or a field that
     * isn't a non-negative number) fail with Invalid Params. */
    struct sb_limits_in lim;
    lim.have        = 0;
    lim.max_rss_kb  = 0;
    lim.max_cpu_ms  = 0;
    lim.max_fds     = 0;
    lim.max_wall_ms = 0;

    const struct json_v *lim_v = json_obj_get(params, "limits");
    if (lim_v) {
        if (lim_v->type != JSON_OBJ) return -32602;
        const struct json_v *f;
        if ((f = json_obj_get(lim_v, "max_rss_kb")) != 0) {
            if (f->type != JSON_NUM) return -32602;
            lim.max_rss_kb = (uint32_t)json_to_int(f);
        }
        if ((f = json_obj_get(lim_v, "max_cpu_ms")) != 0) {
            if (f->type != JSON_NUM) return -32602;
            lim.max_cpu_ms = (uint32_t)json_to_int(f);
        }
        if ((f = json_obj_get(lim_v, "max_fds")) != 0) {
            if (f->type != JSON_NUM) return -32602;
            lim.max_fds = (uint32_t)json_to_int(f);
        }
        if ((f = json_obj_get(lim_v, "max_wall_ms")) != 0) {
            if (f->type != JSON_NUM) return -32602;
            lim.max_wall_ms = (uint32_t)json_to_int(f);
        }
        if (lim.max_rss_kb || lim.max_cpu_ms ||
            lim.max_fds   || lim.max_wall_ms) lim.have = 1;
    }

    return shell_exec_inner(w, params, policy_buf, lim.have ? &lim : 0);
}

/* ============================================================
 * Session 73: KV-store tools
 * ============================================================
 *
 * Five tools on top of libuser's kv_* helpers:
 *
 *   kv.get  {namespace, key}                 -> {value, found}
 *   kv.put  {namespace, key, value}          -> {ok}
 *   kv.del  {namespace, key}                 -> {ok, existed}
 *   kv.list {namespace, prefix}              -> {keys: [...]}
 *   kv.stat {namespace, key}                 -> {size, exists}
 *
 * Path traversal is rejected at the libuser layer (see kv_validate_*),
 * so we don't need to sanitize the ns/key strings here — bad shapes
 * just bubble up as -1 from the underlying kv_* call.
 *
 * Values are JSON strings; binary blobs need client-side base64. The
 * value-field cap is 4 KiB so the wrapping JSON-RPC envelope still
 * fits in agentd's 8 KiB resp[] buffer.
 */

#define KV_VALUE_MAX 4096

static const char *kv_arg_str(const struct json_v *params,
                              const char *field, int *out_len) {
    const struct json_v *v = json_obj_get(params, field);
    return json_to_str(v, out_len);
}

/* nul-copy a json_to_str slice into a fixed C buffer. Caller passes
 * a stack buffer large enough for the longest valid namespace/key. */
static int kv_copy_cstr(char *dst, int cap, const char *s, int slen) {
    if (!s || slen < 0 || slen >= cap) return -1;
    for (int i = 0; i < slen; i++) dst[i] = s[i];
    dst[slen] = 0;
    return 0;
}

static int emit_kv_get(struct json_w *w, const struct json_v *params) {
    int nlen = 0, klen = 0;
    const char *ns = kv_arg_str(params, "namespace", &nlen);
    const char *k  = kv_arg_str(params, "key",       &klen);
    if (!ns || !k) return -32602;

    char ns_c[40], k_c[80];
    if (kv_copy_cstr(ns_c, sizeof(ns_c), ns, nlen) < 0) return -32602;
    if (kv_copy_cstr(k_c,  sizeof(k_c),  k,  klen) < 0) return -32602;

    static char vbuf[KV_VALUE_MAX];
    int n = kv_get(ns_c, k_c, vbuf, sizeof(vbuf));

    json_obj_begin(w);
      json_key(w, "found"); json_bool(w, n >= 0);
      json_key(w, "value");
      if (n >= 0) json_str_n(w, vbuf, n);
      else        json_null(w);
    json_obj_end(w);
    return 0;
}

static int emit_kv_put(struct json_w *w, const struct json_v *params) {
    int nlen = 0, klen = 0, vlen = 0;
    const char *ns = kv_arg_str(params, "namespace", &nlen);
    const char *k  = kv_arg_str(params, "key",       &klen);
    const char *v  = kv_arg_str(params, "value",     &vlen);
    if (!ns || !k || !v) return -32602;
    if (vlen > KV_VALUE_MAX) return -32602;

    char ns_c[40], k_c[80];
    if (kv_copy_cstr(ns_c, sizeof(ns_c), ns, nlen) < 0) return -32602;
    if (kv_copy_cstr(k_c,  sizeof(k_c),  k,  klen) < 0) return -32602;

    int rc = kv_put(ns_c, k_c, v, vlen);
    /* Session 76: notify watchers iff the write actually succeeded.
     * A failed put doesn't change observable state, so we don't
     * pretend it did. Identical value re-writes DO fire — the spec
     * says "the op happened; the watcher decides whether to care",
     * which is cheaper than diffing. */
    if (rc >= 0) kv_notify_change(ns_c, k_c, "put");
    json_obj_begin(w);
      json_key(w, "ok"); json_bool(w, rc >= 0);
    json_obj_end(w);
    return 0;
}

static int emit_kv_del(struct json_w *w, const struct json_v *params) {
    int nlen = 0, klen = 0;
    const char *ns = kv_arg_str(params, "namespace", &nlen);
    const char *k  = kv_arg_str(params, "key",       &klen);
    if (!ns || !k) return -32602;

    char ns_c[40], k_c[80];
    if (kv_copy_cstr(ns_c, sizeof(ns_c), ns, nlen) < 0) return -32602;
    if (kv_copy_cstr(k_c,  sizeof(k_c),  k,  klen) < 0) return -32602;

    /* Peek with stat to determine existed-before. */
    int sz, existed = (kv_stat(ns_c, k_c, &sz) == 0);
    int rc = kv_del(ns_c, k_c);
    /* Session 76: per spec, a kv.del fires the notification even on
     * a no-op (key didn't exist). Watchers that care about
     * distinguishing real-delete from no-op can use the {existed}
     * field on the response. Saves a separate stat-then-notify
     * dance on every write. */
    kv_notify_change(ns_c, k_c, "del");
    json_obj_begin(w);
      json_key(w, "ok");      json_bool(w, rc >= 0);
      json_key(w, "existed"); json_bool(w, existed);
    json_obj_end(w);
    return 0;
}

static int emit_kv_list(struct json_w *w, const struct json_v *params) {
    int nlen = 0, plen = 0;
    const char *ns = kv_arg_str(params, "namespace", &nlen);
    const char *p  = kv_arg_str(params, "prefix",    &plen);   /* optional */
    if (!ns) return -32602;

    char ns_c[40], p_c[80] = {0};
    if (kv_copy_cstr(ns_c, sizeof(ns_c), ns, nlen) < 0) return -32602;
    if (p) {
        if (kv_copy_cstr(p_c, sizeof(p_c), p, plen) < 0) return -32602;
    }

    json_obj_begin(w);
      json_key(w, "keys");
      json_arr_begin(w);
        int  iter = 0;
        char name[16];
        for (int safety = 0; safety < 256; safety++) {
            int r = kv_list(ns_c, p ? p_c : 0, &iter, name);
            if (r < 0) break;
            json_str(w, name);
        }
      json_arr_end(w);
    json_obj_end(w);
    return 0;
}

static int emit_kv_stat(struct json_w *w, const struct json_v *params) {
    int nlen = 0, klen = 0;
    const char *ns = kv_arg_str(params, "namespace", &nlen);
    const char *k  = kv_arg_str(params, "key",       &klen);
    if (!ns || !k) return -32602;

    char ns_c[40], k_c[80];
    if (kv_copy_cstr(ns_c, sizeof(ns_c), ns, nlen) < 0) return -32602;
    if (kv_copy_cstr(k_c,  sizeof(k_c),  k,  klen) < 0) return -32602;

    int  size    = 0;
    int  exists  = (kv_stat(ns_c, k_c, &size) == 0);
    json_obj_begin(w);
      json_key(w, "exists"); json_bool(w, exists);
      json_key(w, "size");   json_int (w, size);
    json_obj_end(w);
    return 0;
}

/* ============================================================
 * Session 76 — kv.watch / kv.unwatch
 * ============================================================
 *
 * Event-driven KV-change subscription. kv.put / kv.del fire
 * `notifications/kv/changed` to every watcher whose (namespace,
 * prefix) tuple matches. Per-connection — close the conn and the
 * watch goes away.
 *
 * Returned watch_id is monotonic across the lifetime of the daemon
 * so it doesn't get accidentally reused if a slot recycles.
 */

static int emit_kv_watch(struct json_w *w, const struct json_v *params) {
    int nlen = 0, plen = 0;
    const char *ns = kv_arg_str(params, "namespace", &nlen);
    const char *p  = kv_arg_str(params, "prefix",    &plen);  /* optional */
    if (!ns) return -32602;
    if (nlen <= 0 || nlen >= KV_WATCH_NS_MAX)        return -32602;
    if (plen <  0 || plen >= KV_WATCH_PREFIX_MAX)    return -32602;
    if (!g_cur) return -32603;

    int slot = -1;
    for (int i = 0; i < MAX_KV_WATCHES; i++) {
        if (!g_kv_watches[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -32603;

    struct kv_watch *kw = &g_kv_watches[slot];
    for (int i = 0; i < nlen; i++) kw->ns[i] = ns[i];
    kw->ns[nlen] = 0;
    if (p && plen > 0) {
        for (int i = 0; i < plen; i++) kw->prefix[i] = p[i];
        kw->prefix[plen] = 0;
    } else {
        kw->prefix[0] = 0;
    }
    kw->conn_fd = g_cur->fd;
    kw->id      = ++g_kv_watch_next_id;
    kw->in_use  = 1;

    json_obj_begin(w);
      json_key(w, "watch_id"); json_int(w, kw->id);
    json_obj_end(w);
    return 0;
}

static int emit_kv_unwatch(struct json_w *w, const struct json_v *params) {
    const struct json_v *wid_v = json_obj_get(params, "watch_id");
    if (!wid_v || wid_v->type != JSON_NUM) return -32602;
    int wid = (int)json_to_int(wid_v);
    if (wid <= 0) return -32602;
    if (!g_cur) return -32603;

    int ok = 0;
    for (int i = 0; i < MAX_KV_WATCHES; i++) {
        if (!g_kv_watches[i].in_use)             continue;
        if (g_kv_watches[i].id != wid)           continue;
        /* Only the conn that registered may unwatch — keeps a
         * misbehaving client from killing another agent's watch. */
        if (g_kv_watches[i].conn_fd != g_cur->fd) continue;
        g_kv_watches[i].in_use = 0;
        ok = 1;
        break;
    }

    json_obj_begin(w);
      json_key(w, "ok"); json_bool(w, ok);
    json_obj_end(w);
    return 0;
}

/* ============================================================
 * Session 74 — background jobs + ring buffers + notifications
 * ============================================================ */

/* Monotonic millisecond counter advanced by the event loop each
 * iteration (10 ms tick). Used for timeout_ms deadlines on the
 * shell.job.wait code path. */
static uint32_t g_tick_ms;

static int  g_listen_fd = -1;

/* Forward decls for notification emit (referenced by the drain code
 * before being defined further down). */
static void emit_job_output_notif(int job_id, int is_stderr,
                                  const char *data, int data_n);
static void emit_job_exit_notif  (int job_id, int exit_code);

/* Append n bytes to the ring's circular buffer, advance `total`.
 * If n > JOB_RING_SZ, only the trailing JOB_RING_SZ bytes are kept;
 * `total` still advances by the full n so readers can detect the
 * skip. */
static void ring_append(struct job_ring *r, const char *data, int n) {
    if (n <= 0) return;
    if (n > JOB_RING_SZ) {
        /* Skip past the bytes that won't fit anyway. */
        int drop = n - JOB_RING_SZ;
        data       += drop;
        n           = JOB_RING_SZ;
        r->total   += (uint32_t)drop;
    }
    uint32_t pos = r->total % JOB_RING_SZ;
    for (int i = 0; i < n; i++) {
        r->buf[pos] = data[i];
        pos = (pos + 1) % JOB_RING_SZ;
    }
    r->total += (uint32_t)n;
}

/* Copy the ring window [from .. min(total, from + cap)) into out.
 * Returns the number of bytes copied. `*next` is set to the new
 * read offset (= from + copied). `*skipped` is set to (window_lo
 * - from) when the caller's `from` predates the oldest retained
 * byte, otherwise 0. */
static int ring_read(const struct job_ring *r, uint32_t from,
                     char *out, int cap,
                     uint32_t *next, uint32_t *skipped) {
    *skipped = 0;
    uint32_t lo = (r->total > JOB_RING_SZ) ? (r->total - JOB_RING_SZ) : 0;
    if (from < lo) { *skipped = lo - from; from = lo; }
    if (from > r->total) from = r->total;
    uint32_t avail = r->total - from;
    int n = (avail > (uint32_t)cap) ? cap : (int)avail;
    uint32_t pos = from % JOB_RING_SZ;
    for (int i = 0; i < n; i++) {
        out[i] = r->buf[pos];
        pos = (pos + 1) % JOB_RING_SZ;
    }
    *next = from + (uint32_t)n;
    return n;
}

/* Find a free job slot (or recycle the oldest fully-drained JS_EXIT
 * slot). Returns slot index or -1 if every slot is RUNNING. */
static int job_alloc_slot(void) {
    for (int i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].state == JS_FREE) return i;
    }
    /* Try recycling an EXIT slot — pick the lowest-index one. */
    for (int i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].state == JS_EXIT) {
            /* Wipe state — any lingering subscribers are abandoned. */
            g_jobs[i].state = JS_FREE;
            return i;
        }
    }
    return -1;
}

static void job_clear(struct job *j) {
    j->state      = JS_FREE;
    j->pid        = 0;
    j->exit_code  = 0;
    j->out_fd     = -1;
    j->err_fd     = -1;
    j->out_eof    = 0;
    j->err_eof    = 0;
    j->reaped     = 0;
    j->out.total  = 0;
    j->err.total  = 0;
    j->cmd_n      = 0;
    j->policy[0]  = 0;
    j->sandboxed  = 0;
    j->sub_mask   = 0;
    j->start_tick = 0;
}

/* Set up two pipes, fork, install limits in child if asked, exec.
 * On success, fills in *out_fd_p / *err_fd_p / *pid_p with the
 * parent-side state and returns 0. Both pipe-read ends are marked
 * non-blocking so the drain loop never stalls. Caller owns the fds
 * thereafter (and must sys_close them on cleanup). */
static int spawn_child(const char *exec_path, const char *const *argv,
                       const struct sb_limits_in *L,
                       int *out_fd_p, int *err_fd_p, int *pid_p) {
    int out_pp[2], err_pp[2];
    if (sys_pipe(out_pp) < 0) return -1;
    if (sys_pipe(err_pp) < 0) {
        sys_close(out_pp[0]); sys_close(out_pp[1]);
        return -1;
    }

    int pid = sys_fork();
    if (pid < 0) {
        sys_close(out_pp[0]); sys_close(out_pp[1]);
        sys_close(err_pp[0]); sys_close(err_pp[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: hook stdout/stderr to the pipes, close stdin
         * (background jobs have no terminal — reads return EOF). */
        sys_dup2(out_pp[1], 1);
        sys_dup2(err_pp[1], 2);
        sys_close(out_pp[0]); sys_close(out_pp[1]);
        sys_close(err_pp[0]); sys_close(err_pp[1]);
        sys_close(0);
        if (L && L->have) {
            struct sys_limits sl;
            sl.max_rss_kb  = L->max_rss_kb;
            sl.max_cpu_ms  = L->max_cpu_ms;
            sl.max_fds     = L->max_fds;
            sl.max_wall_ms = L->max_wall_ms;
            sys_setlimit(&sl);
        }
        sys_exec(exec_path, argv);
        sys_write(2, "agentd-bg: cannot exec\n", 23);
        sys_exit(127);
    }

    /* Parent: drop the write ends, flip both read ends non-blocking. */
    sys_close(out_pp[1]);
    sys_close(err_pp[1]);
    sys_fd_nb(out_pp[0], 1);
    sys_fd_nb(err_pp[0], 1);
    *out_fd_p = out_pp[0];
    *err_fd_p = err_pp[0];
    *pid_p    = pid;
    return 0;
}

/* shell.exec_background tool — spawn a child the same way shell.exec
 * does, but record it in g_jobs[] and return immediately. The two
 * captured streams drain into the slot's ring buffers each event-
 * loop tick; subscribers (via shell.job.subscribe) get pushed bytes
 * as notifications/job.output. Optional `policy` + `limits` work
 * identically to shell.exec_sandboxed. */
static int emit_shell_exec_background(struct json_w *w,
                                      const struct json_v *params) {
    /* cmd / args / optional policy + limits — parsing identical to
     * the synchronous variants above. */
    const struct json_v *cmd_v = json_obj_get(params, "cmd");
    int cmd_len = 0;
    const char *cmd = json_to_str(cmd_v, &cmd_len);
    if (!cmd) return -32602;
    if (cmd_len <= 0 || cmd_len >= JOB_CMD_MAX) return -32602;

    const struct json_v *args_v = json_obj_get(params, "args");
    if (args_v && args_v->type != JSON_ARR) return -32602;
    int n_args = json_arr_len(args_v);
    if (n_args < 0) n_args = 0;
    if (n_args > 12) return -32602;

    /* Optional policy — same templates as shell.exec_sandboxed. */
    const struct json_v *p_v = json_obj_get(params, "policy");
    int p_len = 0;
    const char *policy = p_v ? json_to_str(p_v, &p_len) : 0;
    if (p_v && !policy) return -32602;
    if (policy) {
        if (!(p_len ==  7 && policy[0]=='m' && policy[1]=='i' && policy[2]=='n' &&
                            policy[3]=='i' && policy[4]=='m' && policy[5]=='a' &&
                            policy[6]=='l')
         && !(p_len ==  7 && policy[0]=='c' && policy[1]=='o' && policy[2]=='m' &&
                            policy[3]=='p' && policy[4]=='u' && policy[5]=='t' &&
                            policy[6]=='e')
         && !(p_len ==  6 && policy[0]=='r' && policy[1]=='e' && policy[2]=='a' &&
                            policy[3]=='d' && policy[4]=='f' && policy[5]=='s')
         && !(p_len ==  9 && policy[0]=='n' && policy[1]=='e' && policy[2]=='t' &&
                            policy[3]=='c' && policy[4]=='l' && policy[5]=='i' &&
                            policy[6]=='e' && policy[7]=='n' && policy[8]=='t')) {
            return -32602;
        }
    }

    /* Optional limits object. */
    struct sb_limits_in lim;
    lim.have        = 0;
    lim.max_rss_kb  = 0;
    lim.max_cpu_ms  = 0;
    lim.max_fds     = 0;
    lim.max_wall_ms = 0;
    const struct json_v *lim_v = json_obj_get(params, "limits");
    if (lim_v) {
        if (lim_v->type != JSON_OBJ) return -32602;
        const struct json_v *f;
        if ((f = json_obj_get(lim_v, "max_rss_kb")) != 0) {
            if (f->type != JSON_NUM) return -32602;
            lim.max_rss_kb = (uint32_t)json_to_int(f);
        }
        if ((f = json_obj_get(lim_v, "max_cpu_ms")) != 0) {
            if (f->type != JSON_NUM) return -32602;
            lim.max_cpu_ms = (uint32_t)json_to_int(f);
        }
        if ((f = json_obj_get(lim_v, "max_fds")) != 0) {
            if (f->type != JSON_NUM) return -32602;
            lim.max_fds = (uint32_t)json_to_int(f);
        }
        if ((f = json_obj_get(lim_v, "max_wall_ms")) != 0) {
            if (f->type != JSON_NUM) return -32602;
            lim.max_wall_ms = (uint32_t)json_to_int(f);
        }
        if (lim.max_rss_kb || lim.max_cpu_ms ||
            lim.max_fds   || lim.max_wall_ms) lim.have = 1;
    }

    /* Build argv. Same layout as shell_exec_inner — prepend the
     * sandbox wrapper trio iff a policy was given. */
    const char *argv[20];
    const char *exec_path;
    int ai = 0;
    char policy_buf[JOB_POL_MAX];
    if (policy) {
        for (int i = 0; i < p_len && i < (int)sizeof(policy_buf) - 1; i++)
            policy_buf[i] = policy[i];
        policy_buf[p_len] = 0;
        exec_path  = "sandbox.elf";
        argv[ai++] = "sandbox.elf";
        argv[ai++] = policy_buf;
        argv[ai++] = "--";
    } else {
        exec_path = cmd;
    }
    argv[ai++] = cmd;
    for (int i = 0; i < n_args; i++) {
        const struct json_v *a = json_arr_at(args_v, i);
        int al = 0;
        const char *as = json_to_str(a, &al);
        if (!as) return -32602;
        argv[ai++] = as;
    }
    argv[ai] = 0;

    int slot = job_alloc_slot();
    if (slot < 0) return -32603;  /* table full + nothing to recycle */
    struct job *j = &g_jobs[slot];
    job_clear(j);

    int out_fd, err_fd, pid;
    if (spawn_child(exec_path, argv, lim.have ? &lim : 0,
                    &out_fd, &err_fd, &pid) < 0) {
        return -32603;
    }

    j->state      = JS_RUN;
    j->pid        = pid;
    j->out_fd     = out_fd;
    j->err_fd     = err_fd;
    j->cmd_n      = cmd_len;
    for (int i = 0; i < cmd_len; i++) j->cmd[i] = cmd[i];
    if (policy) {
        j->sandboxed = 1;
        for (int i = 0; i < p_len && i < JOB_POL_MAX - 1; i++)
            j->policy[i] = policy[i];
        j->policy[p_len < JOB_POL_MAX ? p_len : JOB_POL_MAX - 1] = 0;
    }
    j->start_tick = g_tick_ms;

    json_obj_begin(w);
      json_key(w, "job_id"); json_int(w, slot);
      json_key(w, "pid");    json_int(w, pid);
    json_obj_end(w);
    return 0;
}

static int job_id_from_params(const struct json_v *params, int *out) {
    const struct json_v *jv = json_obj_get(params, "job_id");
    if (!jv || jv->type != JSON_NUM) return -32602;
    int id = (int)json_to_int(jv);
    if (id < 0 || id >= JOB_MAX) return -32602;
    if (g_jobs[id].state == JS_FREE) return -32602;
    *out = id;
    return 0;
}

static void emit_job_summary(struct json_w *w, int id) {
    struct job *j = &g_jobs[id];
    json_obj_begin(w);
      json_key(w, "job_id");  json_int(w, id);
      json_key(w, "pid");     json_int(w, j->pid);
      json_key(w, "cmd");     json_str_n(w, j->cmd, j->cmd_n);
      json_key(w, "state");   json_str(w, j->state == JS_RUN ? "running" : "exited");
      json_key(w, "done");    json_bool(w, j->state == JS_EXIT);
      if (j->state == JS_EXIT) {
          json_key(w, "exit_code"); json_int(w, j->exit_code);
      }
      json_key(w, "stdout_total"); json_uint(w, j->out.total);
      json_key(w, "stderr_total"); json_uint(w, j->err.total);
      if (j->sandboxed) {
          int pl = 0; while (j->policy[pl]) pl++;
          json_key(w, "policy"); json_str_n(w, j->policy, pl);
      }
    json_obj_end(w);
}

static int emit_shell_job_list(struct json_w *w, const struct json_v *params) {
    (void)params;
    json_obj_begin(w);
      json_key(w, "jobs");
      json_arr_begin(w);
        for (int i = 0; i < JOB_MAX; i++) {
            if (g_jobs[i].state == JS_FREE) continue;
            emit_job_summary(w, i);
        }
      json_arr_end(w);
    json_obj_end(w);
    return 0;
}

static int emit_shell_job_status(struct json_w *w, const struct json_v *params) {
    int id; int rc = job_id_from_params(params, &id);
    if (rc) return rc;
    emit_job_summary(w, id);
    return 0;
}

/* Maximum bytes returned per shell.job.read call per stream.
 * Bounded by RESP_MAX minus envelope overhead. */
#define JOB_READ_CHUNK_MAX 2048

static int emit_shell_job_read(struct json_w *w, const struct json_v *params) {
    int id; int rc = job_id_from_params(params, &id);
    if (rc) return rc;
    struct job *j = &g_jobs[id];

    uint32_t off_out = 0, off_err = 0;
    const struct json_v *f;
    if ((f = json_obj_get(params, "stdout_offset")) != 0) {
        if (f->type != JSON_NUM) return -32602;
        long lv = json_to_int(f); if (lv < 0) lv = 0;
        off_out = (uint32_t)lv;
    }
    if ((f = json_obj_get(params, "stderr_offset")) != 0) {
        if (f->type != JSON_NUM) return -32602;
        long lv = json_to_int(f); if (lv < 0) lv = 0;
        off_err = (uint32_t)lv;
    }
    int cap = JOB_READ_CHUNK_MAX;
    if ((f = json_obj_get(params, "max")) != 0) {
        if (f->type != JSON_NUM) return -32602;
        long lv = json_to_int(f);
        if (lv > 0 && lv < cap) cap = (int)lv;
    }

    static char obuf[JOB_READ_CHUNK_MAX];
    static char ebuf[JOB_READ_CHUNK_MAX];
    uint32_t out_next = 0, out_skip = 0;
    uint32_t err_next = 0, err_skip = 0;
    int on = ring_read(&j->out, off_out, obuf, cap, &out_next, &out_skip);
    int en = ring_read(&j->err, off_err, ebuf, cap, &err_next, &err_skip);

    json_obj_begin(w);
      json_key(w, "job_id");          json_int(w, id);
      json_key(w, "stdout");          json_str_n(w, obuf, on);
      json_key(w, "stderr");          json_str_n(w, ebuf, en);
      json_key(w, "stdout_next");     json_uint(w, out_next);
      json_key(w, "stderr_next");     json_uint(w, err_next);
      json_key(w, "stdout_total");    json_uint(w, j->out.total);
      json_key(w, "stderr_total");    json_uint(w, j->err.total);
      json_key(w, "stdout_skipped");  json_uint(w, out_skip);
      json_key(w, "stderr_skipped");  json_uint(w, err_skip);
      json_key(w, "done");            json_bool(w, j->state == JS_EXIT);
      if (j->state == JS_EXIT) {
          json_key(w, "exit_code");   json_int(w, j->exit_code);
      }
    json_obj_end(w);
    return 0;
}

/* shell.job.wait is special-cased in dispatch_method to bypass the
 * regular emit-fn machinery — it may transition the conn to
 * CST_PENDING_WAIT and defer the response. The emit-fn slot here is
 * never called (kept NULL in the method table), but the parser is in
 * dispatch_method directly. */

static int emit_shell_job_cancel(struct json_w *w, const struct json_v *params) {
    int id; int rc = job_id_from_params(params, &id);
    if (rc) return rc;
    struct job *j = &g_jobs[id];
    int killed = 0;
    if (j->state == JS_RUN) {
        if (sys_kill(j->pid, SIGKILL) == 0) killed = 1;
        /* Don't wait here — reap_finished_jobs() will pick it up on
         * the next tick and transition state to JS_EXIT. */
    }
    json_obj_begin(w);
      json_key(w, "job_id"); json_int(w, id);
      json_key(w, "killed"); json_bool(w, killed);
    json_obj_end(w);
    return 0;
}

static int emit_shell_job_delete(struct json_w *w, const struct json_v *params) {
    int id; int rc = job_id_from_params(params, &id);
    if (rc) return rc;
    struct job *j = &g_jobs[id];
    if (j->state == JS_RUN) {
        /* Refuse to delete a still-running job — caller must cancel
         * + wait first. Returning "removed: false" lets the agent
         * notice without seeing a JSON-RPC error. */
        json_obj_begin(w);
          json_key(w, "job_id");  json_int(w, id);
          json_key(w, "removed"); json_bool(w, 0);
          json_key(w, "reason");  json_str(w, "still running — cancel first");
        json_obj_end(w);
        return 0;
    }
    /* JS_EXIT — free fds (defensive — already closed) + clear slot. */
    if (j->out_fd >= 0) { sys_close(j->out_fd); j->out_fd = -1; }
    if (j->err_fd >= 0) { sys_close(j->err_fd); j->err_fd = -1; }
    /* Drop this job from every conn's sub_mask. */
    for (int c = 0; c < MAX_CONN; c++) {
        g_conns[c].sub_mask &= ~((uint32_t)1u << id);
    }
    job_clear(j);
    json_obj_begin(w);
      json_key(w, "job_id");  json_int(w, id);
      json_key(w, "removed"); json_bool(w, 1);
    json_obj_end(w);
    return 0;
}

static int emit_shell_job_subscribe(struct json_w *w, const struct json_v *params) {
    int id; int rc = job_id_from_params(params, &id);
    if (rc) return rc;
    int cidx = (int)(g_cur - g_conns);
    if (cidx < 0 || cidx >= MAX_CONN) return -32603;
    g_jobs[id].sub_mask  |=  (uint32_t)1u << cidx;
    g_cur->sub_mask      |=  (uint32_t)1u << id;
    json_obj_begin(w);
      json_key(w, "job_id");       json_int (w, id);
      json_key(w, "subscribed");   json_bool(w, 1);
      json_key(w, "stdout_total"); json_uint(w, g_jobs[id].out.total);
      json_key(w, "stderr_total"); json_uint(w, g_jobs[id].err.total);
    json_obj_end(w);
    return 0;
}

static int emit_shell_job_unsubscribe(struct json_w *w, const struct json_v *params) {
    int id; int rc = job_id_from_params(params, &id);
    if (rc) return rc;
    int cidx = (int)(g_cur - g_conns);
    if (cidx < 0 || cidx >= MAX_CONN) return -32603;
    g_jobs[id].sub_mask  &= ~((uint32_t)1u << cidx);
    g_cur->sub_mask      &= ~((uint32_t)1u << id);
    json_obj_begin(w);
      json_key(w, "job_id");     json_int (w, id);
      json_key(w, "subscribed"); json_bool(w, 0);
    json_obj_end(w);
    return 0;
}

/* ============================================================
 * Tool table — used by both direct and MCP dispatch paths
 * ============================================================ */

struct method {
    const char *name;
    emit_fn     emit;
};

static const struct method g_methods[] = {
    { "time",                 emit_time                 },
    { "getuid",               emit_getuid               },
    { "dns_resolve",          emit_dns_resolve          },
    { "dhcp_info",            emit_dhcp_info            },
    { "dns_cache_stats",      emit_dns_cache_stats      },
    { "fbinfo",               emit_fbinfo               },
    { "smp_stats",            emit_smp_stats            },
    { "shell.exec",           emit_shell_exec           },
    { "shell.exec_sandboxed", emit_shell_exec_sandboxed },
    /* Session 73: persistent agent memory. */
    { "kv.get",               emit_kv_get               },
    { "kv.put",               emit_kv_put               },
    { "kv.del",               emit_kv_del               },
    { "kv.list",              emit_kv_list              },
    { "kv.stat",              emit_kv_stat              },
    /* Session 76: event-driven KV-change subscription. */
    { "kv.watch",             emit_kv_watch             },
    { "kv.unwatch",           emit_kv_unwatch           },
    /* Session 74: streaming background jobs. shell.job.wait is
     * dispatched directly out of dispatch_method() because it may
     * defer its response across event-loop ticks; the rest are
     * plain synchronous emitters. */
    { "shell.exec_background", emit_shell_exec_background },
    { "shell.job.list",        emit_shell_job_list        },
    { "shell.job.status",      emit_shell_job_status      },
    { "shell.job.read",        emit_shell_job_read        },
    { "shell.job.cancel",      emit_shell_job_cancel      },
    { "shell.job.delete",      emit_shell_job_delete      },
    { "shell.job.subscribe",   emit_shell_job_subscribe   },
    { "shell.job.unsubscribe", emit_shell_job_unsubscribe },
    { 0, 0 }
};

static const struct method *find_method(const char *name, int nlen) {
    for (int i = 0; g_methods[i].name; i++) {
        if (str_eq(name, nlen, g_methods[i].name)) return &g_methods[i];
    }
    return 0;
}

static const char *err_msg_for(int code) {
    if (code == -32601) return "Method not found";
    if (code == -32602) return "Invalid params";
    if (code == -32603) return "Internal error";
    return "Server error";
}

/* ============================================================
 * MCP-specific handlers
 * ============================================================ */

static int handle_initialize(const struct json_v *id, const struct json_v *params) {
    /* params carries the client's protocolVersion / capabilities /
     * clientInfo. We don't negotiate — we just announce ours. A
     * future revision could log the client's name+version. */
    (void)params;

    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "protocolVersion"); json_str(&w, MCP_PROTO_VER);
      json_key(&w, "capabilities");
      json_obj_begin(&w);
        /* "tools": {} says "I expose a tools/list and tools/call".
         * Empty object rather than {"listChanged":true} because
         * agentd's tool surface is static — no live add/remove. */
        json_key(&w, "tools");
        json_obj_begin(&w);
        json_obj_end(&w);
        /* Session 73 + 76: advertise the read-only resource surface
         * (resources/list, resources/templates/list, resources/read)
         * PLUS resources/subscribe + resources/unsubscribe (session
         * 76). listChanged stays false — the static set never grows
         * after boot. subscribe flipped to true now that the poll-
         * driven change detector is wired up. */
        json_key(&w, "resources");
        json_obj_begin(&w);
          json_key(&w, "subscribe");   json_bool(&w, 1);
          json_key(&w, "listChanged"); json_bool(&w, 0);
        json_obj_end(&w);
        /* Session 74 + 76: experimental capabilities — vendor
         * extensions clients ignore if they don't know the key. */
        json_key(&w, "experimental");
        json_obj_begin(&w);
          json_key(&w, "adventos.jobs");
          json_obj_begin(&w);
            json_key(&w, "version");          json_int (&w, 1);
            json_key(&w, "max_jobs");         json_int (&w, JOB_MAX);
            json_key(&w, "ring_bytes");       json_int (&w, JOB_RING_SZ);
            json_key(&w, "notifications");
            json_arr_begin(&w);
              json_str(&w, "notifications/job.output");
              json_str(&w, "notifications/job.exit");
            json_arr_end(&w);
          json_obj_end(&w);
          /* Session 76: kv.watch — event-driven KV-change subscription.
           * Notifications fire on every kv.put / kv.del that matches
           * a registered (namespace, prefix). Polling-based equivalents
           * exist for KV via resources/subscribe on kv://NS/KEY URIs
           * but kv.watch is precise (no 200 ms latency) and scoped
           * to a prefix. */
          json_key(&w, "adventos.kv_watch");
          json_obj_begin(&w);
            json_key(&w, "version");                json_int(&w, 1);
            json_key(&w, "max_concurrent_watches"); json_int(&w, MAX_KV_WATCHES);
            json_key(&w, "notifications");
            json_arr_begin(&w);
              json_str(&w, "notifications/kv/changed");
            json_arr_end(&w);
          json_obj_end(&w);
        json_obj_end(&w);
      json_obj_end(&w);
      json_key(&w, "serverInfo");
      json_obj_begin(&w);
        json_key(&w, "name");    json_str(&w, "adventos-agentd");
        json_key(&w, "version"); json_str(&w, "1.0.0");
      json_obj_end(&w);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

/* ============================================================
 * Session 73: MCP resource methods
 * ============================================================
 *
 * MCP's resources/... namespace exposes read-only state with stable
 * URIs and MIME types. Agents that want a structured view of the
 * box's /proc/N and selected /etc/N files use this instead of
 * shell.exec-ing `cat` and parsing stdout.
 *
 * Three methods:
 *   resources/list             enumerate the static URIs
 *   resources/templates/list   parameterized URIs (e.g. /proc/{pid}/.)
 *   resources/read {uri}       fetch the bytes at a URI
 *
 * Static resources are file:// URIs that map 1:1 to FS paths.
 * The kv:// scheme mirrors the kv.get tool — read-only.
 *
 * /etc/passwd is special-cased: hashes get stripped before emit
 * so a future low-privilege agent reading the resource surface
 * doesn't see them. */

struct resource_def {
    const char *uri;
    const char *name;
    const char *mime;
    const char *desc;
};

static const struct resource_def g_resources[] = {
    {"file:///proc/cpuinfo",       "cpuinfo",       "text/plain",
     "CPU vendor / family / features (CPUID-derived)"},
    {"file:///proc/meminfo",       "meminfo",       "text/plain",
     "Total / used / free physical pages"},
    {"file:///proc/uptime",        "uptime",        "text/plain",
     "Seconds since boot (PIT-derived)"},
    {"file:///proc/version",       "version",       "text/plain",
     "Kernel version string"},
    {"file:///proc/mounts",        "mounts",        "text/plain",
     "VFS mount table"},
    {"file:///proc/bcache",        "bcache",        "text/plain",
     "Block-cache hit/miss/writeback counters"},
    {"file:///etc/inittab",        "inittab",       "text/plain",
     "Boot-time service spawn directives"},
    {"file:///etc/passwd",         "passwd",        "text/plain",
     "User table — password hashes redacted on read"},
    {"file:///etc/resolv.conf",    "resolv.conf",   "text/plain",
     "DNS fail-over nameservers"},
    {"file:///etc/agent.tools.json","agent.tools.json","application/json",
     "agentd's tool manifest (the same one served via tools/list)"},
};
#define G_RESOURCES_N ((int)(sizeof(g_resources)/sizeof(g_resources[0])))

struct resource_tpl {
    const char *uri_template;
    const char *name;
    const char *mime;
    const char *desc;
};

static const struct resource_tpl g_templates[] = {
    {"file:///proc/{pid}/status",  "pid/status",  "text/plain",
     "Per-process status (name, pid, ppid, pgid, sid, state)"},
    {"file:///proc/{pid}/sandbox", "pid/sandbox", "text/plain",
     "Sandbox mask + denial ring for a specific task (session 70)"},
    {"file:///proc/{pid}/limits",  "pid/limits",  "text/plain",
     "Resource caps + current usage for a specific task (session 71)"},
    {"kv://{namespace}/{key}",     "kv-entry",    "text/plain",
     "Read-only mirror of kv.get for discoverability"},
};
#define G_TEMPLATES_N ((int)(sizeof(g_templates)/sizeof(g_templates[0])))

static int handle_resources_list(const struct json_v *id, const struct json_v *params) {
    (void)params;
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "resources");
      json_arr_begin(&w);
      for (int i = 0; i < G_RESOURCES_N; i++) {
          json_obj_begin(&w);
            json_key(&w, "uri");         json_str(&w, g_resources[i].uri);
            json_key(&w, "name");        json_str(&w, g_resources[i].name);
            json_key(&w, "mimeType");    json_str(&w, g_resources[i].mime);
            json_key(&w, "description"); json_str(&w, g_resources[i].desc);
          json_obj_end(&w);
      }
      json_arr_end(&w);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

static int handle_resources_templates_list(const struct json_v *id, const struct json_v *params) {
    (void)params;
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "resourceTemplates");
      json_arr_begin(&w);
      for (int i = 0; i < G_TEMPLATES_N; i++) {
          json_obj_begin(&w);
            json_key(&w, "uriTemplate"); json_str(&w, g_templates[i].uri_template);
            json_key(&w, "name");        json_str(&w, g_templates[i].name);
            json_key(&w, "mimeType");    json_str(&w, g_templates[i].mime);
            json_key(&w, "description"); json_str(&w, g_templates[i].desc);
          json_obj_end(&w);
      }
      json_arr_end(&w);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

/* Strip password hashes from a /etc/passwd buffer in place.
 *
 * Format per line: `name:hash:uid:gid:home:shell`.  We replace the
 * hash field with "x" — the same redaction the Linux shadow-password
 * convention uses — and shift the rest of the buffer down.  Returns
 * the new (smaller) length.  Bytewise, in-place, no allocation. */
static int filter_passwd_hashes(char *buf, int n) {
    char tmp[2048];
    int  in_pos  = 0;
    int  out_pos = 0;
    while (in_pos < n && out_pos < (int)sizeof(tmp) - 1) {
        /* Find end of current line (or buffer). */
        int eol = in_pos;
        while (eol < n && buf[eol] != '\n') eol++;
        /* Find the first ':' — separates name from hash. */
        int c1 = in_pos;
        while (c1 < eol && buf[c1] != ':') c1++;
        if (c1 >= eol) {
            /* Malformed line — copy as-is. */
            for (int i = in_pos; i < eol && out_pos < (int)sizeof(tmp) - 1; i++) {
                tmp[out_pos++] = buf[i];
            }
        } else {
            /* Find the second ':' — end of hash field. */
            int c2 = c1 + 1;
            while (c2 < eol && buf[c2] != ':') c2++;
            /* Emit [in_pos..c1] verbatim, ":x", then [c2..eol]. */
            for (int i = in_pos; i <= c1 && out_pos < (int)sizeof(tmp) - 1; i++) {
                tmp[out_pos++] = buf[i];
            }
            if (out_pos < (int)sizeof(tmp) - 1) tmp[out_pos++] = 'x';
            for (int i = c2; i < eol && out_pos < (int)sizeof(tmp) - 1; i++) {
                tmp[out_pos++] = buf[i];
            }
        }
        if (eol < n && out_pos < (int)sizeof(tmp) - 1) {
            tmp[out_pos++] = '\n';
        }
        in_pos = eol + 1;
    }
    int copy_n = out_pos;
    for (int i = 0; i < copy_n; i++) buf[i] = tmp[i];
    return copy_n;
}

#define RES_READ_MAX 4096

/* Match `uri` against the prefix and copy the remainder into `out`
 * (up to cap-1 chars + NUL).  Returns 1 on match, 0 on no-match. */
static int uri_strip_prefix(const char *uri, int ulen,
                            const char *prefix,
                            char *out, int cap) {
    int pl = 0;
    while (prefix[pl]) pl++;
    if (ulen < pl) return 0;
    for (int i = 0; i < pl; i++) if (uri[i] != prefix[i]) return 0;
    int o = 0;
    for (int i = pl; i < ulen && o < cap - 1; i++) out[o++] = uri[i];
    out[o] = 0;
    return 1;
}

/* Session 76: URI-to-bytes resolver, shared by handle_resources_read
 * AND the polling tick that runs change-detection on subscribed URIs.
 *
 * Writes up to `cap-1` bytes plus a NUL into `out`. On success returns
 * the byte count and writes:
 *    *mime_out         "text/plain" or "application/json"
 *    *truncated_out    1 if the read hit cap-1 (more bytes on disk)
 * On URI-not-recognized / missing / read-failure returns -1.
 *
 * Same code path the subscribe poller hashes against. /etc/passwd
 * hash redaction is intentionally applied here too — a subscriber to
 * file:///etc/passwd only sees the redacted text, and the hash that
 * drives notifications is also taken against the redacted bytes (so
 * a behind-the-scenes hash change doesn't leak observable signal).  */
static int read_resource_by_uri(const char *uri, int ulen,
                                char *out, int cap,
                                const char **mime_out,
                                int *truncated_out) {
    if (mime_out)      *mime_out = "text/plain";
    if (truncated_out) *truncated_out = 0;
    if (!uri || ulen <= 0 || cap < 2) return -1;

    char fs_path[128];
    if (uri_strip_prefix(uri, ulen, "file://", fs_path, sizeof(fs_path))) {
        int fd = sys_open(fs_path);
        if (fd < 0) return -1;
        int n = sys_read(fd, out, cap - 1);
        sys_close(fd);
        if (n < 0) n = 0;
        if (n >= cap - 1 && truncated_out) *truncated_out = 1;
        out[n] = 0;
        int content_n = n;

        /* /etc/passwd: redact hashes before emitting. */
        int is_passwd = 1;
        const char *want = "/etc/passwd";
        for (int i = 0; want[i]; i++) {
            if (fs_path[i] != want[i]) { is_passwd = 0; break; }
        }
        if (is_passwd && fs_path[11] == 0) {
            content_n = filter_passwd_hashes(out, content_n);
            out[content_n] = 0;
        }

        /* MIME tag for agent.tools.json. */
        const char *json_path = "/etc/agent.tools.json";
        int match = 1;
        for (int i = 0; json_path[i]; i++) {
            if (fs_path[i] != json_path[i]) { match = 0; break; }
        }
        if (match && fs_path[21] == 0 && mime_out) *mime_out = "application/json";

        return content_n;
    }
    /* kv://<ns>/<key> — read-only mirror of kv.get. */
    char kv_rest[128];
    if (!uri_strip_prefix(uri, ulen, "kv://", kv_rest, sizeof(kv_rest))) {
        return -1;
    }
    int slash = -1;
    for (int i = 0; kv_rest[i]; i++) {
        if (kv_rest[i] == '/') { slash = i; break; }
    }
    if (slash <= 0) return -1;
    kv_rest[slash] = 0;
    int n = kv_get(kv_rest, kv_rest + slash + 1, out, cap - 1);
    if (n < 0) return -1;
    out[n] = 0;
    return n;
}

static int handle_resources_read(const struct json_v *id, const struct json_v *params) {
    int ulen = 0;
    const char *uri = kv_arg_str(params, "uri", &ulen);
    if (!uri) return -32602;

    static char content[RES_READ_MAX];
    const char *mime = "text/plain";
    int truncated = 0;
    int content_n = read_resource_by_uri(uri, ulen, content, sizeof(content),
                                         &mime, &truncated);
    if (content_n < 0) return -32602;

    /* Emit MCP-shaped response:
     *   {"contents": [{"uri": "...", "mimeType": "...", "text": "..."}]} */
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "contents");
      json_arr_begin(&w);
        json_obj_begin(&w);
          json_key(&w, "uri");      json_str_n(&w, uri, ulen);
          json_key(&w, "mimeType"); json_str  (&w, mime);
          json_key(&w, "text");     json_str_n(&w, content, content_n);
          if (truncated) {
              json_key(&w, "truncated"); json_bool(&w, 1);
          }
        json_obj_end(&w);
      json_arr_end(&w);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

/* ============================================================
 * Session 76 — resources/subscribe + resources/unsubscribe
 * ============================================================
 *
 * MCP-builtin methods (not tools — they don't appear in tools/list;
 * clients discover them via capabilities.resources.subscribe=true).
 *
 * subscribe shape:   {uri}            -> {ok: true}
 * unsubscribe shape: {uri}            -> {ok: true}
 *
 * The subscribe path doubles as URI validation: we attempt to
 * read_resource_by_uri once. If that fails we return -32602 and
 * never register the sub. On success we cache the FNV-1a hash so
 * the first poll tick after registration doesn't fire spuriously. */

/* Locate (or create) the g_res_uris slot for `uri`. Returns the
 * slot index or -1 if the table is full. */
static int res_uri_get_or_alloc(const char *uri, int ulen) {
    int free_slot = -1;
    for (int i = 0; i < MAX_RES_URIS; i++) {
        if (!g_res_uris[i].in_use) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (g_res_uris[i].uri_len == ulen) {
            int eq = 1;
            for (int k = 0; k < ulen; k++) {
                if (g_res_uris[i].uri[k] != uri[k]) { eq = 0; break; }
            }
            if (eq) return i;
        }
    }
    if (free_slot < 0) return -1;
    if (ulen >= RES_URI_MAX) return -1;
    for (int k = 0; k < ulen; k++) g_res_uris[free_slot].uri[k] = uri[k];
    g_res_uris[free_slot].uri[ulen] = 0;
    g_res_uris[free_slot].uri_len   = ulen;
    g_res_uris[free_slot].last_hash = 0;
    g_res_uris[free_slot].in_use    = 1;
    return free_slot;
}

/* Drop the slot if no remaining subs point at it. */
static void res_uri_release_if_unused(int uri_idx) {
    if (uri_idx < 0 || uri_idx >= MAX_RES_URIS) return;
    for (int j = 0; j < MAX_RES_SUBS; j++) {
        if (g_res_subs[j].in_use && g_res_subs[j].uri_idx == uri_idx) return;
    }
    g_res_uris[uri_idx].in_use = 0;
    g_res_uris[uri_idx].uri_len = 0;
    g_res_uris[uri_idx].uri[0] = 0;
}

static int handle_resources_subscribe(const struct json_v *id,
                                      const struct json_v *params) {
    int ulen = 0;
    const char *uri = kv_arg_str(params, "uri", &ulen);
    if (!uri || ulen <= 0 || ulen >= RES_URI_MAX) return -32602;
    if (!g_cur) return -32603;

    /* Validate the URI by reading it once. If it doesn't resolve,
     * refuse the subscription rather than silently accepting and
     * never notifying. The read result also seeds last_hash so the
     * first poll-tick compare is same-to-same. */
    static char tmp[RES_POLL_BUF];
    int n = read_resource_by_uri(uri, ulen, tmp, sizeof(tmp), 0, 0);
    if (n < 0) return -32602;

    int uri_idx = res_uri_get_or_alloc(uri, ulen);
    if (uri_idx < 0) {
        return -32603;     /* uri table full */
    }
    g_res_uris[uri_idx].last_hash = fnv1a32(tmp, n);

    /* De-dup (conn, uri) so a careless caller doesn't burn slots.
     * If the pair already exists, treat the call as idempotent and
     * return ok. */
    int conn_fd = g_cur->fd;
    for (int j = 0; j < MAX_RES_SUBS; j++) {
        if (g_res_subs[j].in_use &&
            g_res_subs[j].conn_fd == conn_fd &&
            g_res_subs[j].uri_idx == uri_idx) {
            struct json_w w;
            resp_begin_result(&w, id);
            json_obj_begin(&w);
              json_key(&w, "ok"); json_bool(&w, 1);
            json_obj_end(&w);
            resp_end(&w);
            return json_w_ok(&w) ? 0 : -32603;
        }
    }

    int sub_slot = -1;
    for (int j = 0; j < MAX_RES_SUBS; j++) {
        if (!g_res_subs[j].in_use) { sub_slot = j; break; }
    }
    if (sub_slot < 0) {
        res_uri_release_if_unused(uri_idx);   /* might have just been allocated */
        return -32603;     /* sub table full */
    }
    g_res_subs[sub_slot].in_use  = 1;
    g_res_subs[sub_slot].conn_fd = conn_fd;
    g_res_subs[sub_slot].uri_idx = uri_idx;

    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "ok"); json_bool(&w, 1);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

static int handle_resources_unsubscribe(const struct json_v *id,
                                        const struct json_v *params) {
    int ulen = 0;
    const char *uri = kv_arg_str(params, "uri", &ulen);
    if (!uri || ulen <= 0 || ulen >= RES_URI_MAX) return -32602;
    if (!g_cur) return -32603;

    int removed = 0;
    int conn_fd = g_cur->fd;
    for (int j = 0; j < MAX_RES_SUBS; j++) {
        if (!g_res_subs[j].in_use)           continue;
        if (g_res_subs[j].conn_fd != conn_fd) continue;
        int u = g_res_subs[j].uri_idx;
        if (u < 0 || u >= MAX_RES_URIS) continue;
        if (!g_res_uris[u].in_use)           continue;
        if (g_res_uris[u].uri_len != ulen)   continue;
        int eq = 1;
        for (int k = 0; k < ulen; k++) {
            if (g_res_uris[u].uri[k] != uri[k]) { eq = 0; break; }
        }
        if (!eq) continue;
        g_res_subs[j].in_use = 0;
        res_uri_release_if_unused(u);
        removed = 1;
        break;
    }

    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "ok"); json_bool(&w, removed);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

static int handle_tools_list(const struct json_v *id, const struct json_v *params) {
    (void)params;
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "tools");
      /* g_tools_arr already starts with '[' and ends with ']'. We
       * splice it in as a raw fragment — the bookkeeping in json_w
       * still counts it as one value emit so the surrounding object
       * gets its comma/newline right. */
      json_raw(&w, g_tools_arr, g_tools_arr_len);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

/* Build an MCP tools/call response envelope around `inner` bytes. If
 * `is_error` is set, the envelope's `isError` is true and `inner`
 * holds the human-readable error text (NOT JSON). */
static int emit_tools_call_envelope(const struct json_v *id,
                                    const char *inner, int inner_len,
                                    int is_error) {
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "content");
      json_arr_begin(&w);
        json_obj_begin(&w);
          json_key(&w, "type"); json_str(&w, "text");
          json_key(&w, "text"); json_str_n(&w, inner, inner_len);
        json_obj_end(&w);
      json_arr_end(&w);
      json_key(&w, "isError"); json_bool(&w, is_error);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

/* Session 74 — shell.job.wait may defer across event-loop ticks.
 * Both the direct-dispatch and the MCP tools/call paths funnel into
 * handle_shell_job_wait(); the mcp_wrap flag tells it whether to
 * emit a tools/call envelope or a plain JSON-RPC result. The same
 * flag is stowed on the conn for the deferred-resolver path. */
#define DISPATCH_DEFER  77777

static void capture_id(struct conn *c, const struct json_v *id) {
    c->wait_id_is_num = 0;
    c->wait_id_str_n  = 0;
    if (!id) return;
    if (id->type == JSON_NUM) {
        c->wait_id_is_num = 1;
        c->wait_id_num    = (int)id->num;
    } else if (id->type == JSON_STR) {
        int n = id->str_len;
        if (n > (int)sizeof(c->wait_id_str)) n = (int)sizeof(c->wait_id_str);
        for (int i = 0; i < n; i++) c->wait_id_str[i] = id->str[i];
        c->wait_id_str_n = n;
    }
}

static void emit_saved_id(struct json_w *w, struct conn *c) {
    if (c->wait_id_is_num)         json_int  (w, c->wait_id_num);
    else if (c->wait_id_str_n > 0) json_str_n(w, c->wait_id_str, c->wait_id_str_n);
    else                           json_null (w);
}

/* The actual {exit_code, done, timed_out} body — shared by the
 * immediate-completion path and the deferred resolver. */
static void emit_wait_body(struct json_w *w, int job_id, int timed_out) {
    struct job *j = &g_jobs[job_id];
    json_obj_begin(w);
      json_key(w, "job_id");      json_int (w, job_id);
      if (j->state == JS_EXIT) {
          json_key(w, "exit_code"); json_int(w, j->exit_code);
      }
      json_key(w, "done");        json_bool(w, j->state == JS_EXIT);
      json_key(w, "timed_out");   json_bool(w, timed_out);
    json_obj_end(w);
}

static int handle_shell_job_wait(const struct json_v *id,
                                 const struct json_v *params,
                                 int mcp_wrap) {
    const struct json_v *jv = json_obj_get(params, "job_id");
    if (!jv || jv->type != JSON_NUM) return -32602;
    int job_id = (int)json_to_int(jv);
    if (job_id < 0 || job_id >= JOB_MAX) return -32602;
    if (g_jobs[job_id].state == JS_FREE) return -32602;

    int timeout_ms = (int)json_to_int_or(json_obj_get(params, "timeout_ms"), 5000);
    if (timeout_ms < 0) timeout_ms = 0;
    if (timeout_ms > 300000) timeout_ms = 300000;   /* 5 min hard cap */

    if (g_jobs[job_id].state == JS_EXIT) {
        /* Already finished — emit synchronously. */
        if (mcp_wrap) {
            char inner[256];
            struct json_w iw;
            json_w_init(&iw, inner, sizeof(inner));
            emit_wait_body(&iw, job_id, 0);
            json_w_finish(&iw);
            if (!json_w_ok(&iw)) return -32603;
            return emit_tools_call_envelope(id, inner, json_w_len(&iw), 0);
        }
        struct json_w w;
        resp_begin_result(&w, id);
        emit_wait_body(&w, job_id, 0);
        resp_end(&w);
        return json_w_ok(&w) ? 0 : -32603;
    }

    /* Park the conn — the resolver runs each event-loop tick. */
    capture_id(g_cur, id);
    g_cur->state            = CST_PENDING_WAIT;
    g_cur->wait_job         = job_id;
    g_cur->wait_deadline_ms = g_tick_ms + (uint32_t)timeout_ms;
    g_cur->wait_mcp_wrap    = mcp_wrap;
    return DISPATCH_DEFER;
}

static int handle_tools_call(const struct json_v *id, const struct json_v *params) {
    const struct json_v *name_v = json_obj_get(params, "name");
    const struct json_v *args_v = json_obj_get(params, "arguments");
    int nlen = 0;
    const char *name = json_to_str(name_v, &nlen);
    if (!name) return -32602;

    /* shell.job.wait may defer across ticks — the tools/call envelope
     * is rebuilt by the resolver once the job exits / times out. */
    if (str_eq(name, nlen, "shell.job.wait")) {
        return handle_shell_job_wait(id, args_v, /*mcp_wrap=*/1);
    }

    const struct method *m = find_method(name, nlen);
    /* MCP convention: unknown tool name surfaces as a tools/call
     * "isError" content, not a JSON-RPC -32601. Lets the agent see
     * the error in the content block alongside successful calls. */
    if (!m) {
        const char *msg = "no such tool";
        return emit_tools_call_envelope(id, msg, (int)strlen(msg), 1);
    }

    /* Render the inner result into a stack-local buffer. 4 KiB is
     * enough for every tool except shell.exec with a long output —
     * see SHELL_CAP_MAX. If the emit fails (bad args, runtime
     * error), surface it as MCP error content rather than a JSON-RPC
     * error code: agents want to see the failure in content[], not
     * have it stripped by their transport layer. */
    char inner[4096];
    struct json_w iw;
    json_w_init(&iw, inner, sizeof(inner));
    int rc = m->emit(&iw, args_v);
    json_w_finish(&iw);
    if (rc != 0 || !json_w_ok(&iw)) {
        const char *msg = err_msg_for(rc ? rc : -32603);
        return emit_tools_call_envelope(id, msg, (int)strlen(msg), 1);
    }
    return emit_tools_call_envelope(id, inner, json_w_len(&iw), 0);
}


/* ============================================================
 * Top-level dispatch
 * ============================================================ */

static int dispatch_method(const struct json_v *id,
                           const char *method, int mlen,
                           const struct json_v *params) {
    /* MCP methods first — they have slashes, the direct-tool names
     * don't, so an O(few) name compare is fine without a hash. */
    if (str_eq(method, mlen, "initialize"))  return handle_initialize(id, params);
    if (str_eq(method, mlen, "tools/list"))  return handle_tools_list(id, params);
    if (str_eq(method, mlen, "tools/call"))  return handle_tools_call(id, params);
    /* Session 73: read-only resource surface. */
    if (str_eq(method, mlen, "resources/list"))
        return handle_resources_list(id, params);
    if (str_eq(method, mlen, "resources/templates/list"))
        return handle_resources_templates_list(id, params);
    if (str_eq(method, mlen, "resources/read"))
        return handle_resources_read(id, params);
    /* Session 76: subscribe + unsubscribe — MCP-builtins, not tools. */
    if (str_eq(method, mlen, "resources/subscribe"))
        return handle_resources_subscribe(id, params);
    if (str_eq(method, mlen, "resources/unsubscribe"))
        return handle_resources_unsubscribe(id, params);

    /* Session 74 special: shell.job.wait may defer the response. */
    if (str_eq(method, mlen, "shell.job.wait")) {
        return handle_shell_job_wait(id, params, /*mcp_wrap=*/0);
    }

    /* Legacy direct path: method name == tool name. emit into the
     * JSON-RPC envelope's `result` slot directly. */
    const struct method *m = find_method(method, mlen);
    if (!m) return -32601;

    struct json_w w;
    resp_begin_result(&w, id);
    int rc = m->emit(&w, params);
    resp_end(&w);
    if (rc != 0) return rc;
    if (!json_w_ok(&w)) return -32603;
    return 0;
}

/* Parse + dispatch the request line currently in g_cur->req[0..req_len).
 * Returns:
 *   > 0  bytes of response in g_cur->resp ready to send
 *   = 0  deferred — handler parked the conn in CST_PENDING_WAIT,
 *        no response should be sent yet
 *   < 0  unreachable (errors are turned into response envelopes) */
static int dispatch(int req_len) {
    struct json_v *root = json_parse(g_cur->req, req_len,
                                     g_cur->scratch, sizeof(g_cur->scratch));
    if (!root || root->type != JSON_OBJ) {
        int n = 0;
        resp_error(0, -32700, "Parse error",
                   g_cur->resp, sizeof(g_cur->resp), &n);
        return n;
    }
    const struct json_v *jsonrpc = json_obj_get(root, "jsonrpc");
    const struct json_v *method  = json_obj_get(root, "method");
    const struct json_v *id      = json_obj_get(root, "id");
    const struct json_v *params  = json_obj_get(root, "params");

    int vlen = 0;
    const char *vs = json_to_str(jsonrpc, &vlen);
    if (!vs || !(vlen == 3 && vs[0] == '2' && vs[1] == '.' && vs[2] == '0')) {
        int n = 0;
        resp_error(id, -32600, "Invalid Request",
                   g_cur->resp, sizeof(g_cur->resp), &n);
        return n;
    }
    int mlen = 0;
    const char *ms = json_to_str(method, &mlen);
    if (!ms) {
        int n = 0;
        resp_error(id, -32600, "Invalid Request",
                   g_cur->resp, sizeof(g_cur->resp), &n);
        return n;
    }

    int rc = dispatch_method(id, ms, mlen, params);
    if (rc == DISPATCH_DEFER) return 0;       /* response will come later */
    if (rc == 0) {
        return (int)strlen(g_cur->resp);
    }
    int n = 0;
    resp_error(id, rc, err_msg_for(rc),
               g_cur->resp, sizeof(g_cur->resp), &n);
    return n;
}

/* ============================================================
 * Session 74 — Notifications (server-initiated, no `id` field)
 * ============================================================
 *
 * Both notification types are written DIRECTLY to the subscriber's
 * socket fd (i.e. they bypass the conn's resp[] buffer). That keeps
 * the rule "one in-flight response per conn" intact: notifications
 * only go out when the conn isn't mid-send of a response.
 *
 * Bytes are chunked the same way send_more does — the kernel's
 * tcp_send caps payloads at one MTU per call. A notification fits
 * within NOTIF_BUF (1024 + small slack for headers) so a single
 * sys_write almost always carries the whole frame.
 */

#define NOTIF_BUF        1280       /* envelope + JSON-escaped data    */
#define NOTIF_DATA_CHUNK 512        /* max bytes of pipe data per notif */

/* Returns 1 if a notification may safely go out on this conn right
 * now — slot in use AND no response currently mid-send. */
static int conn_idle_for_notif(const struct conn *c) {
    if (c->state == CST_FREE) return 0;
    if (c->state == CST_SEND) return 0;          /* response mid-send */
    return 1;
}

static void send_notif_to(struct conn *c, const char *buf, int n) {
    if (!conn_idle_for_notif(c)) return;
    /* tcp_send is atomic up to one MTU; agentd notifications stay
     * under 1300 bytes so this normally completes in one syscall.
     * If the socket reports an error / -1, treat the conn as dead. */
    int sent = 0;
    while (sent < n) {
        int wn = sys_write(c->fd, buf + sent, n - sent);
        if (wn <= 0) {
            /* Don't close the conn here — drain phase shouldn't
             * mutate g_conns mid-iteration. The next request
             * read on this fd will report EOF and clean up. */
            return;
        }
        sent += wn;
    }
}

static void emit_job_output_notif(int job_id, int is_stderr,
                                  const char *data, int data_n) {
    /* Build once, fan-out to subscribers. */
    char buf[NOTIF_BUF];
    struct json_w w;
    json_w_init(&w, buf, sizeof(buf));
    json_obj_begin(&w);
      json_key(&w, "jsonrpc"); json_str(&w, "2.0");
      json_key(&w, "method");  json_str(&w, "notifications/job.output");
      json_key(&w, "params");
      json_obj_begin(&w);
        json_key(&w, "job_id"); json_int(&w, job_id);
        json_key(&w, "stream"); json_str(&w, is_stderr ? "stderr" : "stdout");
        json_key(&w, "data");   json_str_n(&w, data, data_n);
      json_obj_end(&w);
    json_obj_end(&w);
    json_w_finish(&w);
    if (!json_w_ok(&w)) return;
    int len = json_w_len(&w);
    if (len + 1 >= (int)sizeof(buf)) return;
    buf[len] = '\n';
    int frame_n = len + 1;

    uint32_t mask = g_jobs[job_id].sub_mask;
    for (int i = 0; i < MAX_CONN; i++) {
        if (!(mask & ((uint32_t)1u << i))) continue;
        send_notif_to(&g_conns[i], buf, frame_n);
    }
}

static void emit_job_exit_notif(int job_id, int exit_code) {
    char buf[NOTIF_BUF];
    struct json_w w;
    json_w_init(&w, buf, sizeof(buf));
    json_obj_begin(&w);
      json_key(&w, "jsonrpc"); json_str(&w, "2.0");
      json_key(&w, "method");  json_str(&w, "notifications/job.exit");
      json_key(&w, "params");
      json_obj_begin(&w);
        json_key(&w, "job_id");    json_int(&w, job_id);
        json_key(&w, "exit_code"); json_int(&w, exit_code);
      json_obj_end(&w);
    json_obj_end(&w);
    json_w_finish(&w);
    if (!json_w_ok(&w)) return;
    int len = json_w_len(&w);
    if (len + 1 >= (int)sizeof(buf)) return;
    buf[len] = '\n';
    int frame_n = len + 1;

    uint32_t mask = g_jobs[job_id].sub_mask;
    for (int i = 0; i < MAX_CONN; i++) {
        if (!(mask & ((uint32_t)1u << i))) continue;
        send_notif_to(&g_conns[i], buf, frame_n);
    }
}

/* Session 76 — find an in-use conn by socket fd. Returns the conn
 * pointer or NULL. Used by the subscriber tables, which key on
 * `conn_fd` (the socket fd, not the array index) so a subscription
 * survives slot reshuffling — though we don't reshuffle today, it
 * keeps the API close to the wire identity. */
static struct conn *conn_by_fd(int fd) {
    if (fd < 0) return 0;
    for (int i = 0; i < MAX_CONN; i++) {
        if (g_conns[i].state != CST_FREE && g_conns[i].fd == fd) {
            return &g_conns[i];
        }
    }
    return 0;
}

/* Session 76 — notifications/resources/updated.
 *
 * Fired by the polling tick when a subscribed URI's content hash
 * changes. The notification carries the URI only; clients call
 * `resources/read` to fetch the new bytes (MCP spec). Fans out to
 * every conn with a matching res_sub.uri_idx. */
static void emit_resources_updated_notif(int uri_idx) {
    if (uri_idx < 0 || uri_idx >= MAX_RES_URIS) return;
    if (!g_res_uris[uri_idx].in_use)            return;

    char buf[NOTIF_BUF];
    struct json_w w;
    json_w_init(&w, buf, sizeof(buf));
    json_obj_begin(&w);
      json_key(&w, "jsonrpc"); json_str(&w, "2.0");
      json_key(&w, "method");  json_str(&w, "notifications/resources/updated");
      json_key(&w, "params");
      json_obj_begin(&w);
        json_key(&w, "uri");
        json_str_n(&w, g_res_uris[uri_idx].uri, g_res_uris[uri_idx].uri_len);
      json_obj_end(&w);
    json_obj_end(&w);
    json_w_finish(&w);
    if (!json_w_ok(&w)) return;
    int len = json_w_len(&w);
    if (len + 1 >= (int)sizeof(buf)) return;
    buf[len] = '\n';
    int frame_n = len + 1;

    for (int j = 0; j < MAX_RES_SUBS; j++) {
        if (!g_res_subs[j].in_use)               continue;
        if (g_res_subs[j].uri_idx != uri_idx)    continue;
        struct conn *c = conn_by_fd(g_res_subs[j].conn_fd);
        if (!c)                                  continue;
        send_notif_to(c, buf, frame_n);
    }
}

/* Session 76 — notifications/kv/changed.
 *
 * Called from emit_kv_put / emit_kv_del after a successful write.
 * Walks the watch table, matches (namespace == ns) AND (key starts
 * with prefix), emits to each watcher's conn. `op` is "put" or "del";
 * the new value is NOT included — clients re-read with `kv.get` if
 * they want it.
 *
 * O(MAX_KV_WATCHES) per write; max 16 watches so worst case is a
 * handful of strcmp's even with a full table. */
static void emit_kv_changed_notif(struct conn *c,
                                  const char *ns,
                                  const char *key,
                                  const char *op) {
    char buf[NOTIF_BUF];
    struct json_w w;
    json_w_init(&w, buf, sizeof(buf));
    json_obj_begin(&w);
      json_key(&w, "jsonrpc"); json_str(&w, "2.0");
      json_key(&w, "method");  json_str(&w, "notifications/kv/changed");
      json_key(&w, "params");
      json_obj_begin(&w);
        json_key(&w, "namespace"); json_str(&w, ns);
        json_key(&w, "key");       json_str(&w, key);
        json_key(&w, "op");        json_str(&w, op);
      json_obj_end(&w);
    json_obj_end(&w);
    json_w_finish(&w);
    if (!json_w_ok(&w)) return;
    int len = json_w_len(&w);
    if (len + 1 >= (int)sizeof(buf)) return;
    buf[len] = '\n';
    send_notif_to(c, buf, len + 1);
}

static int starts_with(const char *s, const char *prefix) {
    for (int i = 0; prefix[i]; i++) {
        if (s[i] != prefix[i]) return 0;
    }
    return 1;
}

static int str_eq_c(const char *a, const char *b) {
    for (int i = 0; a[i] || b[i]; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static void kv_notify_change(const char *ns, const char *key, const char *op) {
    for (int i = 0; i < MAX_KV_WATCHES; i++) {
        if (!g_kv_watches[i].in_use)             continue;
        if (!str_eq_c(g_kv_watches[i].ns, ns))   continue;
        /* Empty prefix matches everything in the namespace. */
        if (g_kv_watches[i].prefix[0] != 0 &&
            !starts_with(key, g_kv_watches[i].prefix)) continue;
        struct conn *c = conn_by_fd(g_kv_watches[i].conn_fd);
        if (!c) continue;
        emit_kv_changed_notif(c, ns, key, op);
    }
}

/* ============================================================
 * Session 74 — Event-loop helpers
 * ============================================================ */

#define SEND_CHUNK   1024

/* Push more bytes of c->resp out to the wire. Each call writes at
 * most SEND_CHUNK to keep the kernel's tcp_send happy (it caps the
 * payload at MTU per call). resp[] is set up by prepare_send() with
 * the trailing newline already appended. Once resp_sent == resp_n
 * the conn returns to CST_IDLE. */
static void try_send_more(struct conn *c) {
    if (c->state != CST_SEND) return;
    int want = c->resp_n - c->resp_sent;
    if (want > SEND_CHUNK) want = SEND_CHUNK;
    int wn = sys_write(c->fd, c->resp + c->resp_sent, want);
    if (wn <= 0) {
        /* Peer dropped or kernel pushback. Clean teardown so the
         * conn slot can be recycled. */
        sys_close(c->fd);
        c->fd    = -1;
        c->state = CST_FREE;
        c->sub_mask = 0;
        return;
    }
    c->resp_sent += wn;
    if (c->resp_sent >= c->resp_n) {
        c->state     = CST_IDLE;
        c->resp_n    = 0;
        c->resp_sent = 0;
    }
}

static void close_conn(struct conn *c) {
    /* Capture fd BEFORE we clear it — the res/kv tables key on the
     * socket fd, not the g_conns index. */
    int dead_fd = c->fd;
    if (c->fd >= 0) sys_close(c->fd);
    c->fd        = -1;
    c->state     = CST_FREE;
    c->req_n     = 0;
    c->resp_n    = 0;
    c->resp_sent = 0;
    /* Drop this conn from every job's subscriber set. */
    int idx = (int)(c - g_conns);
    if (idx >= 0 && idx < MAX_CONN) {
        for (int j = 0; j < JOB_MAX; j++) {
            g_jobs[j].sub_mask &= ~((uint32_t)1u << idx);
        }
    }
    c->sub_mask = 0;

    /* Session 76: drop any resource subs / kv watches owned by this
     * conn. After clearing res_subs we run a single pass that frees
     * any res_uri whose subscriber count went to zero — the polling
     * tick stops re-hashing it the next iteration. */
    if (dead_fd >= 0) {
        for (int j = 0; j < MAX_RES_SUBS; j++) {
            if (g_res_subs[j].in_use && g_res_subs[j].conn_fd == dead_fd) {
                g_res_subs[j].in_use = 0;
            }
        }
        for (int i = 0; i < MAX_RES_URIS; i++) res_uri_release_if_unused(i);
        for (int j = 0; j < MAX_KV_WATCHES; j++) {
            if (g_kv_watches[j].in_use && g_kv_watches[j].conn_fd == dead_fd) {
                g_kv_watches[j].in_use = 0;
            }
        }
    }
}

/* Mark resp[] ready to send (newline-terminated, resp_n includes the
 * newline). The trailing '\n' is the line framing the client parses
 * on. */
static void prepare_send(struct conn *c, int resp_n) {
    if (resp_n <= 0 || resp_n >= (int)sizeof(c->resp) - 1) return;
    c->resp[resp_n] = '\n';
    c->resp_n    = resp_n + 1;
    c->resp_sent = 0;
    c->state     = CST_SEND;
}

/* Drain socket bytes into c->req[], looking for the newline that
 * terminates one JSON-RPC line. If a complete line is buffered,
 * dispatch it. Returns 1 if any work was done (bytes read or line
 * dispatched), 0 otherwise. */
static int try_dispatch_one_request(struct conn *c) {
    if (c->state == CST_SEND) {
        try_send_more(c);
        return 1;
    }
    if (c->state == CST_PENDING_WAIT) {
        /* Don't drain bytes — the client is waiting for the deferred
         * wait response. The resolver fires from the main loop. */
        return 0;
    }
    int avail = (int)sizeof(c->req) - c->req_n;
    if (avail <= 0) { close_conn(c); return 1; }

    int n = sys_read(c->fd, c->req + c->req_n, avail);
    if (n < 0) {
        /* Would-block on FD_FL_NONBLOCK — nothing to do. */
        return 0;
    }
    if (n == 0) {
        /* Peer closed cleanly. */
        close_conn(c);
        return 1;
    }
    c->req_n += n;

    /* Find newline. If multiple lines arrived in one read, we only
     * process the first this tick; the rest stay in req[] for next
     * tick. Keeps fairness across conns. */
    int nl = -1;
    for (int i = 0; i < c->req_n; i++) {
        if (c->req[i] == '\n') { nl = i; break; }
    }
    if (nl < 0) {
        if (c->req_n >= (int)sizeof(c->req)) {
            /* Line longer than our request buffer — pathological. */
            close_conn(c);
        }
        return 1;
    }

    g_cur = c;
    int resp_len = dispatch(nl);
    g_cur = 0;

    /* Consume the line + newline. */
    int consumed  = nl + 1;
    int remaining = c->req_n - consumed;
    for (int i = 0; i < remaining; i++) c->req[i] = c->req[i + consumed];
    c->req_n = remaining;

    if (resp_len > 0) {
        prepare_send(c, resp_len);
        try_send_more(c);
    }
    /* resp_len == 0 means handler parked the conn (CST_PENDING_WAIT
     * already set); no response yet. */
    return 1;
}

/* For each RUNNING job, try non-blocking reads from out_fd / err_fd.
 * Any bytes read are appended to the respective ring AND pushed to
 * every subscriber as notifications/job.output. EOF on a pipe sets
 * the per-side eof flag; we close the fd to release the kernel
 * pipe slot. */
static void drain_one_pipe(struct job *j, int is_stderr) {
    int *fdp = is_stderr ? &j->err_fd : &j->out_fd;
    int *eof = is_stderr ? &j->err_eof : &j->out_eof;
    struct job_ring *ring = is_stderr ? &j->err : &j->out;
    if (*fdp < 0) return;

    for (int iter = 0; iter < 4; iter++) {       /* bounded fairness */
        char buf[NOTIF_DATA_CHUNK];
        int n = sys_read(*fdp, buf, sizeof(buf));
        if (n < 0) return;                       /* would-block */
        if (n == 0) {
            *eof = 1;
            sys_close(*fdp);
            *fdp = -1;
            return;
        }
        int job_id = (int)(j - g_jobs);
        ring_append(ring, buf, n);
        if (j->sub_mask) emit_job_output_notif(job_id, is_stderr, buf, n);
    }
}

static void drain_job_pipes(void) {
    for (int i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].state != JS_RUN) continue;
        drain_one_pipe(&g_jobs[i], 0);
        drain_one_pipe(&g_jobs[i], 1);
    }
}

/* sys_wait_nb reaps any zombie among agentd's children; we match the
 * pid to a g_jobs slot and either set j->reaped (waiting for pipes
 * to drain) or, if both pipes have hit EOF, transition to JS_EXIT.
 *
 * The order is: reap → wait for EOF on both pipes → JS_EXIT. That
 * way a slow drain doesn't lose tail output. */
static void reap_finished_jobs(void) {
    for (int iter = 0; iter < JOB_MAX; iter++) {
        int code = 0;
        int pid  = sys_wait_nb(&code);
        if (pid <= 0) break;                     /* no zombies left */
        for (int i = 0; i < JOB_MAX; i++) {
            if (g_jobs[i].state == JS_RUN && g_jobs[i].pid == pid) {
                g_jobs[i].exit_code = code;
                g_jobs[i].reaped    = 1;
                break;
            }
        }
    }
    /* Promote any reaped job whose pipes are both drained. */
    for (int i = 0; i < JOB_MAX; i++) {
        struct job *j = &g_jobs[i];
        if (j->state != JS_RUN) continue;
        if (!j->reaped) continue;
        if (!j->out_eof || !j->err_eof) continue;
        j->state = JS_EXIT;
        if (j->sub_mask) emit_job_exit_notif(i, j->exit_code);
    }
}

/* Walk CST_PENDING_WAIT conns. If the job has exited or the deadline
 * elapsed, build the deferred response into c->resp and arm send. */
static void process_pending_waits(void) {
    for (int i = 0; i < MAX_CONN; i++) {
        struct conn *c = &g_conns[i];
        if (c->state != CST_PENDING_WAIT) continue;

        int job_id = c->wait_job;
        int done   = (job_id >= 0 && job_id < JOB_MAX &&
                      g_jobs[job_id].state == JS_EXIT);
        int timed  = (!done && (int)(c->wait_deadline_ms - g_tick_ms) <= 0);
        if (!done && !timed) continue;

        g_cur = c;
        if (c->wait_mcp_wrap) {
            /* Render body, then wrap in tools/call envelope. */
            char inner[256];
            struct json_w iw;
            json_w_init(&iw, inner, sizeof(inner));
            emit_wait_body(&iw, job_id, timed);
            json_w_finish(&iw);
            /* Build the envelope manually (emit_tools_call_envelope
             * uses g_cur->resp via resp_begin_result — same id-replay
             * path. We use emit_saved_id here to get the original
             * client id back into the response). */
            struct json_w w;
            json_w_init(&w, c->resp, sizeof(c->resp));
            json_obj_begin(&w);
              json_key(&w, "jsonrpc"); json_str(&w, "2.0");
              json_key(&w, "id");      emit_saved_id(&w, c);
              json_key(&w, "result");
              json_obj_begin(&w);
                json_key(&w, "content");
                json_arr_begin(&w);
                  json_obj_begin(&w);
                    json_key(&w, "type"); json_str(&w, "text");
                    json_key(&w, "text"); json_str_n(&w, inner, json_w_len(&iw));
                  json_obj_end(&w);
                json_arr_end(&w);
                json_key(&w, "isError"); json_bool(&w, 0);
              json_obj_end(&w);
            json_obj_end(&w);
            json_w_finish(&w);
            if (!json_w_ok(&w)) { g_cur = 0; close_conn(c); continue; }
            prepare_send(c, json_w_len(&w));
        } else {
            struct json_w w;
            json_w_init(&w, c->resp, sizeof(c->resp));
            json_obj_begin(&w);
              json_key(&w, "jsonrpc"); json_str(&w, "2.0");
              json_key(&w, "id");      emit_saved_id(&w, c);
              json_key(&w, "result");
              emit_wait_body(&w, job_id, timed);
            json_obj_end(&w);
            json_w_finish(&w);
            if (!json_w_ok(&w)) { g_cur = 0; close_conn(c); continue; }
            prepare_send(c, json_w_len(&w));
        }
        g_cur = 0;
        try_send_more(c);
    }
}

/* Session 76 — re-read every subscribed URI, FNV-1a it, compare to
 * the cached hash. On mismatch, update the cache and fan out a
 * `notifications/resources/updated` to every subscriber.
 *
 * Called from the event loop every 20 ticks (~200 ms). The shared
 * 4 KiB poll buffer caps the hashable region: a procfs file that
 * grows past it has its prefix hashed only (rare today — the bcache
 * + meminfo + ssh_keys files are all far below). A future kprintf-
 * once warning could flag the truncation case if it actually bites.
 *
 * NO immediate notification on first poll after a subscribe — that
 * would be a "fired the moment you subscribed" race the agent
 * doesn't want. handle_resources_subscribe seeds `last_hash` with
 * the freshly-read value at registration time, so the first poll
 * compares same-to-same and stays quiet. RES_POLL_BUF is defined
 * once with the session 76 tables up top. */
static void poll_subscribed_resources(void) {
    static char poll_buf[RES_POLL_BUF];
    for (int i = 0; i < MAX_RES_URIS; i++) {
        if (!g_res_uris[i].in_use) continue;
        int n = read_resource_by_uri(g_res_uris[i].uri,
                                     g_res_uris[i].uri_len,
                                     poll_buf, sizeof(poll_buf), 0, 0);
        if (n < 0) continue;       /* skip unreadable; don't churn cache */
        uint32_t h = fnv1a32(poll_buf, n);
        if (h != g_res_uris[i].last_hash) {
            g_res_uris[i].last_hash = h;
            emit_resources_updated_notif(i);
        }
    }
}

static int add_conn(int fd) {
    for (int i = 0; i < MAX_CONN; i++) {
        if (g_conns[i].state == CST_FREE) {
            g_conns[i].fd        = fd;
            g_conns[i].state     = CST_IDLE;
            g_conns[i].req_n     = 0;
            g_conns[i].resp_n    = 0;
            g_conns[i].resp_sent = 0;
            g_conns[i].sub_mask  = 0;
            return i;
        }
    }
    /* No free slot — refuse the connection. */
    sys_close(fd);
    return -1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    load_tools_manifest();

    /* Initialise the conn + job tables to known zero state. BSS is
     * already zeroed but make the intent explicit. */
    for (int i = 0; i < MAX_CONN; i++) {
        g_conns[i].fd    = -1;
        g_conns[i].state = CST_FREE;
    }
    for (int i = 0; i < JOB_MAX; i++) {
        g_jobs[i].state  = JS_FREE;
        g_jobs[i].out_fd = -1;
        g_jobs[i].err_fd = -1;
    }

    int s = sys_socket();
    if (s < 0)                  { puts("agentd: socket() failed\n"); return 1; }
    if (sys_bind(s, AGENTD_PORT) < 0)
                                { puts("agentd: bind() failed\n");   return 1; }
    if (sys_listen(s, 4) < 0)   { puts("agentd: listen() failed\n"); return 1; }
    /* Non-blocking listener so sys_accept doesn't park the loop. */
    sys_fd_nb(s, 1);
    g_listen_fd = s;

    /* listening — silent */

    for (;;) {
        /* 1. Accept any new connection. */
        int c = sys_accept(g_listen_fd);
        if (c >= 0) {
            sys_fd_nb(c, 1);
            add_conn(c);
        }

        /* 2. Service each conn: send pending response bytes or
         *    pick up the next request line. */
        for (int i = 0; i < MAX_CONN; i++) {
            if (g_conns[i].state != CST_FREE) {
                try_dispatch_one_request(&g_conns[i]);
            }
        }

        /* 3. Drain any output piling up from background jobs and
         *    push it to subscribers as notifications.            */
        drain_job_pipes();

        /* 4. Reap any exited child, transition RUN -> EXIT, emit
         *    notifications/job.exit to subscribers.              */
        reap_finished_jobs();

        /* 5. Resolve any conn parked on shell.job.wait. */
        process_pending_waits();

        /* 6. Session 76: poll-driven resource-change detection.
         *    Every 20 ticks (~200 ms) re-fetch + hash + diff every
         *    subscribed URI. The cadence is the worst-case latency
         *    for a notifications/resources/updated push; faster
         *    would burn CPU on volatile URIs like /proc/uptime. */
        static uint32_t g_poll_div;
        if (++g_poll_div >= 20) {
            g_poll_div = 0;
            poll_subscribed_resources();
        }

        /* 7. Yield. 10 ms tick lets sense-of-realtime stay tight
         *    without burning the CPU when nothing's happening.   */
        sys_sleep_ms(10);
        g_tick_ms += 10;
    }
    return 0;
}
