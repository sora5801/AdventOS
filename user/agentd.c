/*
 * agentd — JSON-RPC 2.0 tool surface for external agents.
 *
 *   tcp://127.0.0.1:7000   newline-framed JSON-RPC
 *
 * Connect, send one request per line, read one response per line,
 * close (or keep-alive — agentd reads until EOF on each connection).
 *
 *   {"jsonrpc":"2.0","id":1,"method":"time"}                  →
 *     {"jsonrpc":"2.0","id":1,"result":{"epoch":1714680000}}
 *
 *   {"jsonrpc":"2.0","id":2,"method":"shell.exec",
 *    "params":{"cmd":"ls","args":["/etc"]}}                   →
 *     {"jsonrpc":"2.0","id":2,"result":{"stdout":"inittab\npasswd\n...","stderr":"","exit_code":0}}
 *
 *   {"jsonrpc":"2.0","id":3,"method":"bogus"}                 →
 *     {"jsonrpc":"2.0","id":3,"error":{"code":-32601,"message":"Method not found"}}
 *
 * Methods (mirror /etc/agent.tools.json):
 *
 *   time                 -> {epoch}
 *   getuid               -> {uid}
 *   dns_resolve(host)    -> {ip}
 *   dhcp_info            -> {ip, netmask, gateway, dns_server, lease_seconds}
 *   dns_cache_stats      -> {lookups, hits, misses, evictions}
 *   fbinfo               -> {width, height, bpp, pitch} OR {enabled:false}
 *   smp_stats            -> {nr_cpus, ticks:[...]}
 *   shell.exec(cmd,args) -> {stdout, stderr, exit_code}
 *
 * Auth model: loopback-only. agentd binds 127.0.0.1, so the only way
 * to hit it is from inside the OS (e.g. an SSH session, which is
 * already authenticated by sshd). No per-connection credential.
 * shell.exec inherits agentd's uid (= 0, since init starts agentd
 * directly) — same trust level as anyone who can already get a shell.
 *
 * Framing: one JSON document per line, terminated by '\n'. Requests
 * larger than REQ_MAX bytes get rejected. The server emits one
 * response per accepted request, also '\n'-terminated. This is the
 * "framed JSON" subset of JSON-RPC over a stream — strictly simpler
 * than full Content-Length framing, fine for trusted loopback.
 *
 * Concurrency: fork-per-connection. The parent loops on accept(),
 * the child handles the connection and exits. Zombies get drained
 * non-blocking between accept calls.
 */
#include "libuser.h"
#include "../libjson/libjson.h"

#define AGENTD_PORT     7000
#define REQ_MAX         4096      /* per-request line cap */
#define RESP_MAX        8192      /* per-response cap */
#define SCRATCH_MAX     6144      /* JSON parser arena */
#define SHELL_CAP_MAX   3072      /* shell.exec captured stdout/stderr cap */

/* ============================================================
 * Per-connection state
 * ============================================================
 */

struct conn {
    int  fd;
    char req[REQ_MAX];
    int  req_n;
    char resp[RESP_MAX];
    char scratch[SCRATCH_MAX];
};

/* The struct is ~50 KiB, larger than the default 4 KiB user stack.
 * Move it to BSS so each fork()-child gets its own copy without
 * needing to grow the stack. */
static struct conn g_c;

/* ============================================================
 * Response builders
 * ============================================================ */

static void resp_begin_result(struct json_w *w, const struct json_v *id) {
    json_w_init(w, g_c.resp, sizeof(g_c.resp));
    json_obj_begin(w);
      json_key(w, "jsonrpc"); json_str(w, "2.0");
      json_key(w, "id");
      if (!id || id->type == JSON_NULL) {
          json_null(w);
      } else if (id->type == JSON_NUM) {
          json_int(w, (int)id->num);
      } else if (id->type == JSON_STR) {
          json_str_n(w, id->str, id->str_len);
      } else {
          json_null(w);
      }
      json_key(w, "result");
}

static void resp_end(struct json_w *w) {
    json_obj_end(w);
    /* NUL-terminate at the actual response length. Without this,
     * dispatch's `strlen(g_c.resp)` would walk past our content
     * into leftover bytes from the PREVIOUS request — same buffer
     * is reused across the per-connection loop and never zeroed,
     * so unterminated tails leaked into responses. */
    json_w_finish(w);
}

static void resp_error(const struct json_v *id, int code, const char *msg,
                       char *out_buf, int out_cap, int *out_n) {
    struct json_w w;
    json_w_init(&w, out_buf, out_cap);
    json_obj_begin(&w);
      json_key(&w, "jsonrpc"); json_str(&w, "2.0");
      json_key(&w, "id");
      if (!id || id->type == JSON_NULL) {
          json_null(&w);
      } else if (id->type == JSON_NUM) {
          json_int(&w, (int)id->num);
      } else if (id->type == JSON_STR) {
          json_str_n(&w, id->str, id->str_len);
      } else {
          json_null(&w);
      }
      json_key(&w, "error");
      json_obj_begin(&w);
        json_key(&w, "code");    json_int(&w, code);
        json_key(&w, "message"); json_str(&w, msg);
      json_obj_end(&w);
    json_obj_end(&w);
    json_w_finish(&w);    /* NUL-terminate; see resp_end. */
    *out_n = json_w_len(&w);
}

/* ============================================================
 * Method handlers — each takes the parsed params (may be NULL) and
 * produces a response by writing into g_c.resp via json_w.
 *
 * Return 0 on success, JSON-RPC error code (negative) on failure
 * (in which case the caller emits the error envelope).
 * ============================================================ */

static int handle_time(const struct json_v *id, const struct json_v *params) {
    (void)params;
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "epoch"); json_uint(&w, sys_time());
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;   /* Internal error */
}

static int handle_getuid(const struct json_v *id, const struct json_v *params) {
    (void)params;
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "uid"); json_int(&w, sys_getuid());
      json_key(&w, "gid"); json_int(&w, sys_getgid());
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

static int handle_dns_resolve(const struct json_v *id, const struct json_v *params) {
    const struct json_v *hv = json_obj_get(params, "host");
    int hlen = 0;
    const char *hs = json_to_str(hv, &hlen);
    if (!hs || hlen <= 0 || hlen > 250) return -32602;   /* Invalid params */

    /* json string is NUL-terminated thanks to libjson's arena layout. */
    unsigned char ip[4];
    if (sys_dns_resolve(hs, ip) < 0) return -32000;      /* Server error */

    /* Format dotted-quad. */
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

    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "ip"); json_str_n(&w, dq, o);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
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

static int handle_dhcp_info(const struct json_v *id, const struct json_v *params) {
    (void)params;
    struct sys_dhcp_info di;
    sys_dhcp_info(&di);

    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "have_lease");      json_bool(&w, di.have_lease);
      json_key(&w, "ip");              emit_dotted_quad(&w, di.ip);
      json_key(&w, "netmask");         emit_dotted_quad(&w, di.netmask);
      json_key(&w, "gateway");         emit_dotted_quad(&w, di.gateway);
      json_key(&w, "dns_server");      emit_dotted_quad(&w, di.dns_server);
      json_key(&w, "lease_seconds");   json_uint(&w, di.lease_seconds);
      json_key(&w, "acquired_epoch");  json_uint(&w, di.acquired_epoch);
      json_key(&w, "t1_renew_at");     json_uint(&w, di.t1_renew_at);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

static int handle_dns_cache_stats(const struct json_v *id, const struct json_v *params) {
    (void)params;
    unsigned int out[4] = {0};
    sys_dns_cache_stats(out);
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "lookups");    json_uint(&w, out[0]);
      json_key(&w, "hits");       json_uint(&w, out[1]);
      json_key(&w, "misses");     json_uint(&w, out[2]);
      json_key(&w, "evictions");  json_uint(&w, out[3]);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

static int handle_fbinfo(const struct json_v *id, const struct json_v *params) {
    (void)params;
    unsigned int info[4] = {0};
    int on = sys_fbinfo(info);
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      if (on > 0) {
          json_key(&w, "enabled"); json_bool(&w, 1);
          json_key(&w, "width");   json_uint(&w, info[0]);
          json_key(&w, "height");  json_uint(&w, info[1]);
          json_key(&w, "bpp");     json_uint(&w, info[2]);
          json_key(&w, "pitch");   json_uint(&w, info[3]);
      } else {
          json_key(&w, "enabled"); json_bool(&w, 0);
      }
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

static int handle_smp_stats(const struct json_v *id, const struct json_v *params) {
    (void)params;
    unsigned int out[8] = {0};
    sys_smp_stats(out);
    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "nr_cpus"); json_uint(&w, out[0]);
      json_key(&w, "ticks");
      json_arr_begin(&w);
        /* The remaining slots are per-CPU tick counts (out[1..]).
         * Length = out[0]; capped at 7 by SMP_STATS layout. */
        unsigned int n = out[0];
        if (n > 7) n = 7;
        for (unsigned int i = 0; i < n; i++) json_uint(&w, out[1 + i]);
      json_arr_end(&w);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

/* ----- shell.exec helpers ----- */

/* Slurp from `fd` into `buf` until EOF or cap-1. NUL-terminates.
 * Returns bytes read. */
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

static int handle_shell_exec(const struct json_v *id, const struct json_v *params) {
    /* params: {"cmd":"<exe>","args":[...optional...]} */
    const struct json_v *cmd_v = json_obj_get(params, "cmd");
    int cmd_len = 0;
    const char *cmd = json_to_str(cmd_v, &cmd_len);
    if (!cmd) return -32602;

    const struct json_v *args_v = json_obj_get(params, "args");
    if (args_v && args_v->type != JSON_ARR) return -32602;
    int n_args = json_arr_len(args_v);
    if (n_args < 0) n_args = 0;
    /* argv = cmd + args + NULL. Caps the total argv at 16. */
    if (n_args > 15) return -32602;
    const char *argv[17];
    argv[0] = cmd;
    int ai = 1;
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
        /* Child: wire pipes onto stdout/stderr, close all the
         * unrelated fds, exec. */
        sys_dup2(out_pp[1], 1);
        sys_dup2(err_pp[1], 2);
        sys_close(out_pp[0]); sys_close(out_pp[1]);
        sys_close(err_pp[0]); sys_close(err_pp[1]);
        sys_exec(cmd, argv);
        /* exec returned → not found. */
        sys_write(2, "agentd-exec: cannot exec\n", 25);
        sys_exit(127);
    }

    /* Parent: close write ends so child's EOF actually arrives. */
    sys_close(out_pp[1]);
    sys_close(err_pp[1]);

    static char out_buf[SHELL_CAP_MAX];
    static char err_buf[SHELL_CAP_MAX];
    /* Drain stdout first. With small commands this is fine; for big
     * outputs the child might block on its stderr because we're not
     * draining both at once. shell.exec is for short commands —
     * ls, ps, cat of small configs — so we accept that constraint. */
    int out_n = drain_fd(out_pp[0], out_buf, sizeof(out_buf));
    int err_n = drain_fd(err_pp[0], err_buf, sizeof(err_buf));
    sys_close(out_pp[0]);
    sys_close(err_pp[0]);

    int code = 0;
    sys_wait(&code);

    struct json_w w;
    resp_begin_result(&w, id);
    json_obj_begin(&w);
      json_key(&w, "exit_code"); json_int(&w, code);
      json_key(&w, "stdout");    json_str_n(&w, out_buf, out_n);
      json_key(&w, "stderr");    json_str_n(&w, err_buf, err_n);
    json_obj_end(&w);
    resp_end(&w);
    return json_w_ok(&w) ? 0 : -32603;
}

/* ============================================================
 * Dispatch
 * ============================================================ */

struct method {
    const char *name;
    int (*fn)(const struct json_v *id, const struct json_v *params);
};

static const struct method g_methods[] = {
    { "time",             handle_time             },
    { "getuid",           handle_getuid           },
    { "dns_resolve",      handle_dns_resolve      },
    { "dhcp_info",        handle_dhcp_info        },
    { "dns_cache_stats",  handle_dns_cache_stats  },
    { "fbinfo",           handle_fbinfo           },
    { "smp_stats",        handle_smp_stats        },
    { "shell.exec",       handle_shell_exec       },
    { 0, 0 }
};

static int str_eq(const char *a, int an, const char *b) {
    int bn = 0; while (b[bn]) bn++;
    if (an != bn) return 0;
    for (int i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Parse one request line in g_c.req[0..g_c.req_n) and emit the
 * response into g_c.resp; returns response length. */
static int dispatch(void) {
    struct json_v *root = json_parse(g_c.req, g_c.req_n,
                                     g_c.scratch, sizeof(g_c.scratch));
    if (!root || root->type != JSON_OBJ) {
        int n = 0;
        resp_error(0, -32700, "Parse error", g_c.resp, sizeof(g_c.resp), &n);
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

    for (int i = 0; g_methods[i].name; i++) {
        if (str_eq(ms, mlen, g_methods[i].name)) {
            int rc = g_methods[i].fn(id, params);
            if (rc == 0) {
                /* Response built in g_c.resp by the handler. */
                return (int)strlen(g_c.resp);
            }
            int n = 0;
            const char *msg =
                rc == -32602 ? "Invalid params" :
                rc == -32603 ? "Internal error" :
                "Server error";
            resp_error(id, rc, msg, g_c.resp, sizeof(g_c.resp), &n);
            return n;
        }
    }

    int n = 0;
    resp_error(id, -32601, "Method not found",
               g_c.resp, sizeof(g_c.resp), &n);
    return n;
}

/* ============================================================
 * Connection loop
 * ============================================================ */

/* NUL-terminate the response and send it with a trailing newline.
 * The response buffer is mutated to end with '\n\0'. */
static void send_response(int fd, int n) {
    if (n < 0 || n >= (int)sizeof(g_c.resp) - 1) return;
    g_c.resp[n] = '\n';
    sys_write(fd, g_c.resp, n + 1);
}

/* Read bytes from fd into g_c.req until we have a complete line (\n)
 * or we hit EOF / overflow. Returns:
 *   >0 : length of one request (no \n included)
 *    0 : peer closed cleanly
 *   -1 : overflow or error
 */
static int read_one_request(int fd) {
    g_c.req_n = 0;
    while (g_c.req_n < (int)sizeof(g_c.req) - 1) {
        char c;
        int n = sys_read(fd, &c, 1);
        if (n <= 0) {
            /* EOF after partial line: nothing to dispatch. */
            return g_c.req_n == 0 ? 0 : -1;
        }
        if (c == '\n') return g_c.req_n;
        g_c.req[g_c.req_n++] = c;
    }
    return -1;
}

static void handle_client(int conn) {
    /* Per-connection: loop on read_one_request until EOF. Lets agents
     * pipeline several requests over a single TCP connection without
     * paying the TCP-handshake cost each time. */
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

    int s = sys_socket();
    if (s < 0)                  { puts("agentd: socket() failed\n"); return 1; }
    if (sys_bind(s, AGENTD_PORT) < 0)
                                { puts("agentd: bind() failed\n");   return 1; }
    if (sys_listen(s, 4) < 0)   { puts("agentd: listen() failed\n"); return 1; }

    printf("agentd: listening on 127.0.0.1:%d (JSON-RPC 2.0)\n",
           (int)AGENTD_PORT);

    /* Sequential, one-connection-at-a-time accept loop — the same
     * shape httpd uses. Agents on AdventOS aren't expected to issue
     * concurrent requests; one in flight at a time is fine and avoids
     * the fork-per-connection footprint (each fork would CoW agentd's
     * BSS, magnifying its memory cost). shell.exec internally still
     * forks/execs the user-named command — that's where the
     * concurrency we DO need lives. */
    for (;;) {
        int conn = sys_accept(s);
        if (conn < 0) continue;
        handle_client(conn);
    }
    return 0;
}
