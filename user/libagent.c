/*
 * libagent — see libagent.h for the public surface.
 */
#include "libagent.h"

static int la_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

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
        if (r == 0) return 0;          /* EOF */
        /* r < 0 — would-block. Sleep a bit and retry. */
        if (waited >= timeout_ms) return -1;
        sys_sleep_ms(10);
        waited += 10;
    }
    buf[n] = 0;
    return -1;
}

int agent_call(const char *json_request, char *resp_buf, int resp_cap) {
    int sk = connect_localhost_7000();
    if (sk < 0) return -1;
    if (agent_send_line(sk, json_request) < 0) {
        sys_close(sk);
        return -1;
    }
    int n = agent_recv_line(sk, resp_buf, resp_cap);
    sys_close(sk);
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
