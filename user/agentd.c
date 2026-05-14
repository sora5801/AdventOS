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
 * Same eight tools (time, getuid, dns_resolve, dhcp_info,
 * dns_cache_stats, fbinfo, smp_stats, shell.exec) reachable either
 * way. MCP-aware clients (Claude Desktop, Claude Code agents) talk
 * tools/call; the session-64 selftest still pokes the direct path.
 *
 * Architecture:
 *
 *   ┌────────────────────────────┐
 *   │ accept loop (sequential)   │
 *   └──┬─────────────────────────┘
 *      │ per-connection req/resp loop
 *      ▼
 *   ┌────────────────────────────┐
 *   │ dispatch(method) routes:   │
 *   │   "initialize"  → handshake reply (protocolVersion, capabilities,
 *   │                                    serverInfo)
 *   │   "tools/list"  → emit cached manifest "tools" array
 *   │   "tools/call"  → look up tool by name, render its emit_fn into
 *   │                   a scratch buffer, wrap in MCP content block
 *   │   any other     → look up in g_methods, run emit_fn into the
 *   │                   JSON-RPC envelope directly                  │
 *   └────────────────────────────┘
 *
 * Each tool is one `emit_fn(json_w *, json_v *params)` that writes
 * its `{...}` result body. The direct path wraps it as `{"result": ...}`
 * inside the JSON-RPC envelope; the tools/call path renders it into
 * `g_c.inner`, then JSON-encodes that buffer as a string inside an
 * MCP `{"content":[{"type":"text","text":"..."}],"isError":false}`.
 *
 * Manifest: /etc/agent.tools.json is read at startup. We extract
 * its `tools` array and cache its re-emitted JSON bytes — tools/list
 * just splices those bytes into the response.
 *
 * Loopback only — uid 0. No auth. Anyone reaching 127.0.0.1:7000
 * already has shell-level privileges via sshd. See docs/65-mcp-server.md.
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
 * INNER_MAX   scratch for tools/call inner-result rendering
 * MANIFEST_MAX cap on the raw file and the re-emitted tools array
 * SHELL_CAP_MAX  per-stream limit on shell.exec capture */
/* Match session 64's sizes — those flew through the selftest without
 * destabilising any other test. The MCP additions don't need
 * permanently-resident inner buffers: tools/call's inner-result
 * staging buffer is stack-local in the handler, and the manifest
 * loader's scratch is stack-local at boot. */
#define REQ_MAX         4096
#define RESP_MAX        8192
#define SCRATCH_MAX     8192    /* session 71 bump: parsing the bigger
                                 * manifest needs room for both the raw
                                 * bytes (~5 KiB now with the limits
                                 * schema) and the libjson value tree. */
#define MANIFEST_MAX    8192    /* was 4096; manifest grew past it when
                                 * shell.exec_sandboxed + limits were
                                 * added — sys_read truncated mid-JSON
                                 * and the parser saw a truncated tail. */
#define SHELL_CAP_MAX   2048

struct conn {
    int  fd;
    char req[REQ_MAX];
    int  req_n;
    char resp[RESP_MAX];
    char scratch[SCRATCH_MAX];
};

static struct conn g_c;

/* Manifest cache. Populated once at boot from /etc/agent.tools.json
 * via load_tools_manifest; tools/list splices g_tools_arr into its
 * response. Re-emitted (not raw file bytes) so the JSON is parser-
 * round-tripped — guaranteed well-formed, no embedded whitespace
 * surprises, no comments to handle. */
/* The re-emitted tools array — ~3.5 KiB now with 9 tools and the
 * limits-object schema on shell.exec_sandboxed. Sized to comfortably
 * hold the manifest plus the libjson re-emit envelope. */
static char g_tools_arr[8192];
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
     * resident forever. The scratch arena is the libjson parser's
     * working memory — needs to hold one `struct json_v` per JSON
     * value (8 tools × ~10 nodes each + array wrapping ≈ 90 nodes ×
     * ~32 bytes = 3 KiB) plus all decoded string bodies (~2 KiB).
     * 16 KiB gives headroom for future tool additions. Stack budget
     * is 64 KiB (USER_STACK_PAGES=16), so 4+16 KiB is comfortable. */
    char raw[MANIFEST_MAX];
    char scratch[16384];

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

/* Initialise w over g_c.resp and write the envelope opening up to
 * "result":, leaving the caller to emit the result value (an
 * object or scalar) and then call resp_end(). */
static void resp_begin_result(struct json_w *w, const struct json_v *id) {
    json_w_init(w, g_c.resp, sizeof(g_c.resp));
    json_obj_begin(w);
      json_key(w, "jsonrpc"); json_str(w, "2.0");
      json_key(w, "id");      emit_id(w, id);
      json_key(w, "result");
}

static void resp_end(struct json_w *w) {
    json_obj_end(w);
    /* NUL-terminate at the actual response length. Without this,
     * dispatch's strlen(g_c.resp) would walk into bytes left over
     * from the previous request — same buffer reused across the
     * per-connection loop, never zeroed. */
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

static int handle_tools_call(const struct json_v *id, const struct json_v *params) {
    const struct json_v *name_v = json_obj_get(params, "name");
    const struct json_v *args_v = json_obj_get(params, "arguments");
    int nlen = 0;
    const char *name = json_to_str(name_v, &nlen);
    if (!name) return -32602;

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

static int dispatch(void) {
    struct json_v *root = json_parse(g_c.req, g_c.req_n,
                                     g_c.scratch, sizeof(g_c.scratch));
    if (!root || root->type != JSON_OBJ) {
        int n = 0;
        resp_error(0, -32700, "Parse error",
                   g_c.resp, sizeof(g_c.resp), &n);
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
                   g_c.resp, sizeof(g_c.resp), &n);
        return n;
    }
    int mlen = 0;
    const char *ms = json_to_str(method, &mlen);
    if (!ms) {
        int n = 0;
        resp_error(id, -32600, "Invalid Request",
                   g_c.resp, sizeof(g_c.resp), &n);
        return n;
    }

    int rc = dispatch_method(id, ms, mlen, params);
    if (rc == 0) {
        /* Response already in g_c.resp, NUL-terminated by resp_end. */
        return (int)strlen(g_c.resp);
    }
    int n = 0;
    resp_error(id, rc, err_msg_for(rc),
               g_c.resp, sizeof(g_c.resp), &n);
    return n;
}

/* ============================================================
 * Connection loop
 * ============================================================ */

/* Chunked write — the kernel's tcp_send refuses payloads larger than
 * one MTU-1480-byte segment in a single call (kernel/tcp.c uses a
 * 1500-byte stack scratch buffer), and sys_write doesn't fragment for
 * us. tools/list and shell.exec responses can run 1.6+ KiB, so we
 * break the response into ≤1024-byte chunks. The agent's TCP stack
 * reassembles transparently. */
#define SEND_CHUNK   1024

static void send_response(int fd, int n) {
    if (n < 0 || n >= (int)sizeof(g_c.resp) - 1) return;
    g_c.resp[n] = '\n';
    int total = n + 1;
    int sent  = 0;
    while (sent < total) {
        int want = total - sent;
        if (want > SEND_CHUNK) want = SEND_CHUNK;
        int wn = sys_write(fd, g_c.resp + sent, want);
        if (wn <= 0) return;     /* peer closed / error — drop the rest */
        sent += wn;
    }
}

static int read_one_request(int fd) {
    g_c.req_n = 0;
    while (g_c.req_n < (int)sizeof(g_c.req) - 1) {
        char c;
        int n = sys_read(fd, &c, 1);
        if (n <= 0) return g_c.req_n == 0 ? 0 : -1;
        if (c == '\n') return g_c.req_n;
        g_c.req[g_c.req_n++] = c;
    }
    return -1;
}

static void handle_client(int conn) {
    for (;;) {
        int req_n = read_one_request(conn);
        if (req_n <= 0) break;
        int resp_n = dispatch();
        send_response(conn, resp_n);
    }
    sys_close(conn);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    load_tools_manifest();

    int s = sys_socket();
    if (s < 0)                  { puts("agentd: socket() failed\n"); return 1; }
    if (sys_bind(s, AGENTD_PORT) < 0)
                                { puts("agentd: bind() failed\n");   return 1; }
    if (sys_listen(s, 4) < 0)   { puts("agentd: listen() failed\n"); return 1; }

    /* listening — silent */

    for (;;) {
        int conn = sys_accept(s);
        if (conn < 0) continue;
        handle_client(conn);
    }
    return 0;
}
