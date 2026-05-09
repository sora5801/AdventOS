/*
 * wget — minimal HTTP/1.0 downloader.
 *
 *   wget http://localhost/                - print body to stdout
 *   wget http://example.com/index.html
 *   wget http://localhost/ -O page.txt    - save body to file (tmpfs)
 *
 * Parses the URL, opens a TCP connection, sends a one-line HTTP/1.0
 * GET, reads everything until EOF, splits headers from body on the
 * first blank line, and either writes the body to stdout or to a
 * tmpfs file via sys_open_w. Status code is parsed from the first
 * response line and reported on stderr ("HTTP 200" / "HTTP 404").
 *
 * No HTTPS, no redirects, no chunked transfer (HTTP/1.0 has none),
 * no Content-Length sanity checking. Connection: close is implicit
 * because we're 1.0.
 */

#include "libuser.h"

#define DEFAULT_PORT  80
#define HOST_MAX      64
#define PATH_MAX      128

/* Parse a URL of the form "http://HOST[:PORT]/PATH" into components.
 * Returns 0 on success and fills host/port/path. */
static int parse_url(const char *url, char *host, int *port, char *path) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;

    /* Host. Stop at ':' or '/'. */
    int hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < HOST_MAX - 1) host[hi++] = *p++;
    host[hi] = 0;
    if (hi == 0) return -1;

    /* Optional :PORT. */
    *port = DEFAULT_PORT;
    if (*p == ':') {
        p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (v <= 0 || v > 65535) return -1;
        *port = v;
    }

    /* Path — at minimum "/". */
    int pi = 0;
    if (*p != '/') path[pi++] = '/';
    while (*p && pi < PATH_MAX - 1) path[pi++] = *p++;
    path[pi] = 0;
    return 0;
}

static int parse_ipv4(const char *s, unsigned char ip[4]) {
    int v[4] = {0,0,0,0};
    int seg  = 0, digits = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            v[seg] = v[seg] * 10 + (*s - '0');
            if (v[seg] > 255) return 0;
            digits++;
        } else if (*s == '.') {
            if (digits == 0) return 0;
            seg++;
            if (seg > 3) return 0;
            digits = 0;
        } else { return 0; }
        s++;
    }
    if (seg != 3 || digits == 0) return 0;
    for (int i = 0; i < 4; i++) ip[i] = (unsigned char)v[i];
    return 1;
}

static int resolve(const char *host, unsigned char ip[4]) {
    if (parse_ipv4(host, ip)) return 0;
    if (strcmp(host, "localhost") == 0) {
        ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1;
        return 0;
    }
    return sys_dns_resolve(host, ip);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write(2, "wget: usage: wget URL [-O FILE]\n", 32);
        return 2;
    }

    char host[HOST_MAX];
    int  port;
    char path[PATH_MAX];
    if (parse_url(argv[1], host, &port, path) < 0) {
        sys_write(2, "wget: bad URL\n", 14);
        return 1;
    }

    const char *outfile = 0;
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-O") == 0) { outfile = argv[i + 1]; break; }
    }

    unsigned char ip[4];
    if (resolve(host, ip) < 0) {
        sys_write(2, "wget: DNS resolve failed for ", 29);
        sys_write(2, host, (int)strlen(host));
        sys_write(2, "\n", 1);
        return 1;
    }

    int sk = sys_socket();
    if (sk < 0) { sys_write(2, "wget: socket failed\n", 20); return 1; }
    if (sys_connect(sk, ip, port) < 0) {
        sys_write(2, "wget: connect failed\n", 21);
        sys_close(sk);
        return 1;
    }

    /* Send the request. HTTP/1.0 + Host header is enough for most
     * naive servers (and for our httpd, which ignores headers). */
    char req[256];
    int  ri = 0;
    const char *parts[] = {
        "GET ", path, " HTTP/1.0\r\nHost: ", host, "\r\nConnection: close\r\n\r\n", 0
    };
    for (int p = 0; parts[p]; p++) {
        const char *s = parts[p];
        while (*s && ri < (int)sizeof(req)) req[ri++] = *s++;
    }
    sys_write(sk, req, ri);

    /* Slurp the whole response. Split at the first \r\n\r\n. */
    char rbuf[1024];
    int  total = 0;
    int  in_body = 0;
    int  outfd = -1;
    if (outfile) {
        outfd = sys_open_w(outfile);
        if (outfd < 0) {
            sys_write(2, "wget: cannot open output\n", 25);
            sys_close(sk);
            return 1;
        }
    }
    int  sink = (outfd >= 0) ? outfd : 1;
    int  status = 0;

    /* For the first chunk we keep a small running window so we can
     * detect "\r\n\r\n" even when it straddles two reads. */
    char hdrbuf[1024];
    int  hdrlen = 0;

    int  n;
    while ((n = sys_read(sk, rbuf, sizeof(rbuf))) > 0) {
        if (!in_body) {
            for (int i = 0; i < n; i++) {
                if (hdrlen < (int)sizeof(hdrbuf)) hdrbuf[hdrlen++] = rbuf[i];
                /* End-of-headers detection. */
                if (hdrlen >= 4 &&
                    hdrbuf[hdrlen-4] == '\r' && hdrbuf[hdrlen-3] == '\n' &&
                    hdrbuf[hdrlen-2] == '\r' && hdrbuf[hdrlen-1] == '\n') {
                    in_body = 1;
                    /* Parse status from "HTTP/x.y NNN" first line. */
                    for (int j = 0; j < hdrlen - 4; j++) {
                        if (hdrbuf[j] == ' ') {
                            int v = 0;
                            int k = j + 1;
                            while (k < hdrlen - 4 &&
                                   hdrbuf[k] >= '0' && hdrbuf[k] <= '9') {
                                v = v * 10 + (hdrbuf[k] - '0');
                                k++;
                            }
                            status = v;
                            break;
                        }
                    }
                    /* Pipe the body remnant of this chunk to sink. */
                    int tail_off = i + 1;
                    int tail_len = n - tail_off;
                    if (tail_len > 0) {
                        sys_write(sink, rbuf + tail_off, tail_len);
                        total += tail_len;
                    }
                    break;
                }
            }
        } else {
            sys_write(sink, rbuf, n);
            total += n;
        }
    }
    sys_close(sk);
    if (outfd >= 0) sys_close(outfd);

    /* Status line on stderr — diagnostic, doesn't pollute the body. */
    char msg[64];
    int  mi = 0;
    const char *pre = "wget: HTTP ";
    while (*pre) msg[mi++] = *pre++;
    int s = (status > 0) ? status : 0;
    char tmp[8]; int ti = 0;
    if (s == 0) tmp[ti++] = '?';
    while (s) { tmp[ti++] = (char)('0' + s % 10); s /= 10; }
    while (ti--) msg[mi++] = tmp[ti];
    const char *suf = "  body=";
    while (*suf) msg[mi++] = *suf++;
    int b = total;
    char btmp[8]; int bi = 0;
    if (b == 0) btmp[bi++] = '0';
    while (b) { btmp[bi++] = (char)('0' + b % 10); b /= 10; }
    while (bi--) msg[mi++] = btmp[bi];
    msg[mi++] = '\n';
    sys_write(2, msg, mi);

    return (status >= 200 && status < 400) ? 0 : 1;
}
