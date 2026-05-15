/*
 * agentctl — in-guest CLI for the session-74 background-jobs API.
 *
 * The host-side path (nc / Python -> 127.0.0.1:7000) is unreliable
 * under -serial stdio on Windows/MSYS2 (paste truncation, RST on
 * close). agentctl runs inside the guest, opens a TCP socket to
 * 127.0.0.1:7000, sends one or more JSON-RPC requests, and prints
 * the responses. Same wire shape an external agent would use, but
 * driven from the in-guest shell.
 *
 * Usage:
 *   agentctl demo            scripted end-to-end test
 *   agentctl bg CMD          shell.exec_background {cmd}
 *   agentctl list            shell.job.list
 *   agentctl status ID       shell.job.status {id}
 *   agentctl read   ID       shell.job.read   {id}
 *   agentctl wait   ID [MS]  shell.job.wait   {id, timeout_ms}
 *   agentctl cancel ID       shell.job.cancel {id}
 *   agentctl delete ID       shell.job.delete {id}
 *   agentctl sub    ID       shell.job.subscribe + read 1 line then exit
 *
 * Quoting: bg only forwards the bare cmd token (no args). For more
 * complex invocations use a tools/call envelope by hand.
 */

#include "libuser.h"

#define AGENTD_PORT 7000

/* Tiny "send a line, read a line" helper. Lines are short JSON. */
static int g_sk = -1;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int connect_agentd(void) {
    unsigned char ip[4] = {127, 0, 0, 1};
    int sk = sys_socket();
    if (sk < 0) { puts("agentctl: socket failed\n"); return -1; }
    if (sys_connect(sk, ip, AGENTD_PORT) < 0) {
        puts("agentctl: connect failed (is agentd running?)\n");
        sys_close(sk);
        return -1;
    }
    return sk;
}

static int send_line(const char *s) {
    int n = slen(s);
    int sent = 0;
    while (sent < n) {
        int w = sys_write(g_sk, s + sent, n - sent);
        if (w <= 0) return -1;
        sent += w;
    }
    if (sys_write(g_sk, "\n", 1) <= 0) return -1;
    return 0;
}

/* Print one newline-terminated frame from the socket to stdout. */
static int recv_line_print(void) {
    char c;
    int  total = 0;
    for (;;) {
        int n = sys_read(g_sk, &c, 1);
        if (n <= 0) return total > 0 ? total : -1;
        sys_write(1, &c, 1);
        total++;
        if (c == '\n') return total;
    }
}

/* Build a small JSON-RPC request line in `out`. `params_json` is the
 * raw JSON object body (without surrounding braces), or 0 for {}. */
static int build_req(char *out, int cap,
                     int id, const char *method, const char *params_json) {
    int i = 0;
    const char *p1 = "{\"jsonrpc\":\"2.0\",\"id\":";
    while (*p1 && i < cap - 1) out[i++] = *p1++;
    /* id as integer */
    char tmp[12]; int ti = 0;
    int v = id; if (v == 0) tmp[ti++] = '0';
    else { while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; } }
    while (ti) out[i++] = tmp[--ti];
    const char *p2 = ",\"method\":\"";
    while (*p2 && i < cap - 1) out[i++] = *p2++;
    while (*method && i < cap - 1) out[i++] = *method++;
    out[i++] = '"';
    if (params_json) {
        const char *p3 = ",\"params\":";
        while (*p3 && i < cap - 1) out[i++] = *p3++;
        while (*params_json && i < cap - 1) out[i++] = *params_json++;
    }
    if (i < cap - 1) out[i++] = '}';
    out[i] = 0;
    return i;
}

static int cmd_simple(const char *method, const char *params_json) {
    g_sk = connect_agentd();
    if (g_sk < 0) return 1;
    char req[512];
    build_req(req, sizeof(req), 1, method, params_json);
    send_line(req);
    recv_line_print();
    sys_close(g_sk);
    return 0;
}

static int cmd_bg(const char *cmd) {
    char params[256];
    int  i = 0;
    const char *a = "{\"cmd\":\"";
    while (*a) params[i++] = *a++;
    while (*cmd) params[i++] = *cmd++;
    params[i++] = '"';
    params[i++] = '}';
    params[i] = 0;
    return cmd_simple("shell.exec_background", params);
}

static int cmd_with_id(const char *method, const char *id_str) {
    char params[64];
    int i = 0;
    const char *a = "{\"job_id\":";
    while (*a) params[i++] = *a++;
    while (*id_str) params[i++] = *id_str++;
    params[i++] = '}';
    params[i] = 0;
    return cmd_simple(method, params);
}

static int cmd_wait(const char *id_str, const char *ms_str) {
    char params[96];
    int i = 0;
    const char *a = "{\"job_id\":";
    while (*a) params[i++] = *a++;
    while (*id_str) params[i++] = *id_str++;
    const char *b = ",\"timeout_ms\":";
    while (*b) params[i++] = *b++;
    while (*ms_str) params[i++] = *ms_str++;
    params[i++] = '}';
    params[i] = 0;
    return cmd_simple("shell.job.wait", params);
}

static int cmd_subscribe(const char *id_str) {
    g_sk = connect_agentd();
    if (g_sk < 0) return 1;
    char req[128];
    /* Subscribe. */
    char params[32]; int pi = 0;
    const char *a = "{\"job_id\":";
    while (*a) params[pi++] = *a++;
    while (*id_str) params[pi++] = *id_str++;
    params[pi++] = '}';
    params[pi] = 0;
    build_req(req, sizeof(req), 1, "shell.job.subscribe", params);
    send_line(req);
    puts("subscribe: ");
    recv_line_print();
    /* Read incoming notifications until the conn drops or we hit
     * the loop cap. 32 lines is enough for a smoke test. */
    puts("notifications:\n");
    for (int i = 0; i < 32; i++) {
        if (recv_line_print() < 0) break;
    }
    sys_close(g_sk);
    return 0;
}

static int cmd_demo(void) {
    puts("--- agentctl demo: shell.exec_background + read + wait ---\n");
    g_sk = connect_agentd();
    if (g_sk < 0) return 1;

    char req[256];

    /* 1. initialize (handshake). */
    build_req(req, sizeof(req), 1, "initialize", "{}");
    send_line(req);
    puts("init: "); recv_line_print();

    /* 2. spawn a background ls.elf. */
    build_req(req, sizeof(req), 2, "shell.exec_background",
              "{\"cmd\":\"ls.elf\"}");
    send_line(req);
    puts("bg:   "); recv_line_print();

    /* 3. wait up to 3 seconds for it to finish. */
    build_req(req, sizeof(req), 3, "shell.job.wait",
              "{\"job_id\":0,\"timeout_ms\":3000}");
    send_line(req);
    puts("wait: "); recv_line_print();

    /* 4. read whatever it produced. */
    build_req(req, sizeof(req), 4, "shell.job.read", "{\"job_id\":0}");
    send_line(req);
    puts("read: "); recv_line_print();

    /* 5. list jobs (should show one in EXIT state). */
    build_req(req, sizeof(req), 5, "shell.job.list", "{}");
    send_line(req);
    puts("list: "); recv_line_print();

    /* 6. delete the slot. */
    build_req(req, sizeof(req), 6, "shell.job.delete", "{\"job_id\":0}");
    send_line(req);
    puts("del:  "); recv_line_print();

    /* 7. list again (should be empty). */
    build_req(req, sizeof(req), 7, "shell.job.list", "{}");
    send_line(req);
    puts("list: "); recv_line_print();

    sys_close(g_sk);
    puts("--- demo complete ---\n");
    return 0;
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void usage(void) {
    puts("usage:\n"
         "  agentctl demo\n"
         "  agentctl bg CMD\n"
         "  agentctl list\n"
         "  agentctl status ID\n"
         "  agentctl read   ID\n"
         "  agentctl wait   ID [MS]\n"
         "  agentctl cancel ID\n"
         "  agentctl delete ID\n"
         "  agentctl sub    ID\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (streq(argv[1], "demo"))     return cmd_demo();
    if (streq(argv[1], "list"))     return cmd_simple("shell.job.list", "{}");
    if (streq(argv[1], "bg")) {
        if (argc < 3) { usage(); return 1; }
        return cmd_bg(argv[2]);
    }
    if (argc < 3) { usage(); return 1; }
    if (streq(argv[1], "status"))   return cmd_with_id("shell.job.status", argv[2]);
    if (streq(argv[1], "read"))     return cmd_with_id("shell.job.read",   argv[2]);
    if (streq(argv[1], "cancel"))   return cmd_with_id("shell.job.cancel", argv[2]);
    if (streq(argv[1], "delete"))   return cmd_with_id("shell.job.delete", argv[2]);
    if (streq(argv[1], "sub"))      return cmd_subscribe(argv[2]);
    if (streq(argv[1], "wait")) {
        const char *ms = (argc >= 4) ? argv[3] : "5000";
        return cmd_wait(argv[2], ms);
    }
    puts("agentctl: unknown subcommand\n");
    usage();
    return 1;
}
