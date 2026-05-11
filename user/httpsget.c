/*
 * httpsget — HTTPS client. Now does *real-world* HTTPS GETs:
 * resolve hostname → connect TCP → TLS 1.3 handshake with SNI →
 * HTTP/1.1 GET with proper Host header → drain response → print.
 *
 * Usage:
 *   httpsget                              defaults to https://10.0.2.15:4433/
 *                                         (the in-OS httpsd loopback test)
 *   httpsget https://example.com/         hits a real public server
 *   httpsget example.com                  scheme defaults to https
 *   httpsget example.com /path
 *
 * The TLS client accepts any cert without verification (the
 * curl -k equivalent), but the handshake itself is fully RFC 8446:
 * ClientHello with SNI + signature_algorithms covering ECDSA + RSA-
 * PSS + Ed25519, X25519 ECDHE, AES-128-GCM record protection. That's
 * enough for the demo to reach the real internet through QEMU SLIRP
 * NAT and pull back a live page.
 *
 * No HTTPS redirects, no chunked transfer decoding (still HTTP/1.0
 * over the response — most public hosts cooperate with that), no
 * TLS session resumption, no HTTP/2.
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/tls.h"
#include "../libcrypto/x509.h"

#define HOST_MAX 128
#define PATH_MAX 256

/* libuser only exposes strlen / strcmp / strncmp — these tiny copies
 * keep httpsget self-contained without growing the ABI. */
static char *str_cpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++) != 0) {}
    return dst;
}
static void str_ncpy(char *dst, const char *src, int n) {
    int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}
/* Just enough sprintf for the request builder — %s and %d. Returns
 * bytes written (excluding the trailing NUL, capped at cap-1). */
static int small_sprintf(char *out, int cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int o = 0;
    for (const char *f = fmt; *f; f++) {
        if (o >= cap - 1) break;
        if (*f != '%') { out[o++] = *f; continue; }
        f++;
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            while (*s && o < cap - 1) out[o++] = *s++;
        } else if (*f == 'd') {
            int v = va_arg(ap, int);
            char buf[12]; int bi = 0;
            int neg = (v < 0); if (neg) v = -v;
            if (v == 0) buf[bi++] = '0';
            else while (v) { buf[bi++] = (char)('0' + v % 10); v /= 10; }
            if (neg && o < cap - 1) out[o++] = '-';
            while (bi-- > 0 && o < cap - 1) out[o++] = buf[bi];
        } else if (*f) {
            out[o++] = *f;
        }
    }
    va_end(ap);
    out[o] = 0;
    return o;
}

/* Parse a URL of the form [https://]HOST[:PORT][/PATH]. */
static int parse_url(const char *url, char *host, int *port, char *path) {
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    int hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < HOST_MAX - 1) host[hi++] = *p++;
    host[hi] = 0;
    if (hi == 0) return -1;

    *port = 443;
    if (*p == ':') {
        p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (v <= 0 || v > 65535) return -1;
        *port = v;
    }

    int pi = 0;
    if (*p != '/') path[pi++] = '/';
    while (*p && pi < PATH_MAX - 1) path[pi++] = *p++;
    path[pi] = 0;
    return 0;
}

/* Resolve a host string. Accepts "1.2.3.4" or "name.tld". */
static int resolve_host(const char *host, unsigned char ip[4]) {
    /* If it parses cleanly as a dotted quad, use it directly. */
    int idx = 0, v = 0, digits = 0;
    const char *p = host;
    while (*p) {
        if (*p == '.') {
            if (idx >= 4 || digits == 0) goto via_dns;
            ip[idx++] = (unsigned char)v;
            v = 0; digits = 0;
        } else if (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            digits++;
            if (v > 255) goto via_dns;
        } else {
            goto via_dns;
        }
        p++;
    }
    if (idx == 3 && digits > 0) {
        ip[3] = (unsigned char)v;
        return 0;
    }
via_dns:
    return sys_dns_resolve(host, ip);
}

int main(int argc, char **argv) {
    char host[HOST_MAX];
    char path[PATH_MAX];
    int  port;

    if (argc < 2) {
        /* Backward-compat default: the OS-internal httpsd loopback test. */
        str_cpy(host, "10.0.2.15");
        port = 4433;
        str_cpy(path, "/");
    } else {
        const char *url = argv[1];
        /* Two-argument form "httpsget host path" — convenience. */
        if (argc >= 3 && strncmp(url, "http", 4) != 0) {
            str_ncpy(host, url, HOST_MAX - 1); host[HOST_MAX - 1] = 0;
            port = 443;
            str_ncpy(path, argv[2], PATH_MAX - 1); path[PATH_MAX - 1] = 0;
        } else if (parse_url(url, host, &port, path) < 0) {
            printf("httpsget: cannot parse URL '%s'\n", url);
            return 1;
        }
    }

    unsigned char ip[4] = {0};
    if (resolve_host(host, ip) < 0) {
        printf("httpsget: DNS failed for '%s'\n", host);
        return 1;
    }
    printf("httpsget: %s -> %d.%d.%d.%d:%d %s\n",
           host, ip[0], ip[1], ip[2], ip[3], port, path);

    int sk = sys_socket();
    if (sk < 0) { puts("httpsget: socket failed\n"); return 1; }
    if (sys_connect(sk, ip, port) < 0) {
        puts("httpsget: TCP connect failed\n");
        sys_close(sk);
        return 1;
    }
    puts("httpsget: TCP connected\n");

    /* Tell the TLS layer the SNI before handshaking. SNI should be
     * a DNS name; sending an IP literal is technically allowed by
     * RFC 6066 but a few servers (including Cloudflare's IP-only
     * 1.1.1.1) reject it. Skip SNI if the host looks like a dotted
     * quad — let the server pick a default-vhost cert. */
    int is_ip = 1;
    for (const char *p = host; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) { is_ip = 0; break; }
    }
    /* Session 59 — opt-in CA chain validation.  If /etc/ssl/ contains
     * any trusted CA cert, load them all into a store and hand the
     * TLS layer a pointer; the layer will reject any presented cert
     * that doesn't chain.  When the dir is empty (or the binary is
     * run on a host with no trust store) we fall back to the
     * traditional "trust any cert" behavior so we can still hit real
     * public servers — the safety call is at the caller's level. */
    static struct ca_store store;
    ca_store_init(&store);
    int n_ca = ca_store_load_from_etc_ssl(&store);
    if (n_ca > 0) {
        printf("httpsget: loaded %d CA root(s) from /etc/ssl/ — chain validation ON\n",
               n_ca);
    } else {
        puts("httpsget: no CA roots found in /etc/ssl/ — chain validation OFF\n");
    }

    struct tls_conn t;
    t.server_name = is_ip ? 0 : host;
    /* Pass the store only if we actually loaded something — otherwise
     * a stale empty store would reject every connection. */
    t.ca_store     = n_ca > 0 ? &store : 0;
    t.ca_store_now = 0;       /* RTC isn't NTP-synced yet (session 60 work) */
    int hs = tls_client_handshake_cert(&t, sk);
    if (hs != 0) {
        /* rc=-115 specifically means x509_verify_chain rejected the
         * presented cert.  Call it out explicitly so the t42 selftest
         * can tell "the chain check did its job" from "the handshake
         * blew up for some other reason." */
        if (hs == -115) {
            puts("httpsget: chain validation REJECTED server cert\n");
        }
        printf("httpsget: TLS handshake failed (rc=%d)\n", hs);
        sys_close(sk);
        return 1;
    }
    puts("httpsget: TLS 1.3 handshake OK\n");

    /* HTTP/1.0 request — short, no chunked transfer, "Connection:
     * close" implicit. The Host: header is what every public server
     * keys vhost selection on. */
    char req[512];
    int n = small_sprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: AdventOS-httpsget/1.0\r\n"
                     "Accept: */*\r\n"
                     "\r\n",
                     path, host);
    if (tls_send(&t, req, n) < 0) {
        puts("httpsget: tls_send failed\n");
        sys_close(sk);
        return 1;
    }
    printf("httpsget: sent %d-byte request encrypted\n", n);

    /* Drain the response. Each tls_recv pulls one AEAD record's
     * worth of plaintext. Stop on EOF / error / a soft cap so we
     * don't spew megabytes for a heavy page. */
    int total = 0;
    int header_done = 0;
    const int cap = 8 * 1024;
    char chunk[1536];
    for (;;) {
        int r = tls_recv(&t, chunk, sizeof(chunk) - 1);
        if (r <= 0) break;
        chunk[r] = 0;
        for (int i = 0; i < r; i++) putchar(chunk[i]);
        total += r;
        if (!header_done) {
            /* Just a status indicator after we've seen the header. */
            for (int i = 0; i < r - 3; i++) {
                if (chunk[i] == '\r' && chunk[i+1] == '\n' &&
                    chunk[i+2] == '\r' && chunk[i+3] == '\n') {
                    header_done = 1; break;
                }
            }
        }
        if (total >= cap) break;
    }
    printf("\nhttpsget: received %d plaintext bytes total\n", total);

    sys_close(sk);
    return 0;
}
