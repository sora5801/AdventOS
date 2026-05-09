/*
 * nc — minimal netcat. Connects to host:port and bidirectionally
 * relays bytes between stdin and the socket until either side
 * closes.
 *
 *   nc google.com 80                  - DNS-resolved outbound TCP
 *   nc 127.0.0.1 80                   - dotted-quad outbound
 *   echo "GET / HTTP/1.0\r\n\r\n" | nc localhost 80    - one-shot HTTP
 *
 * No flags today: no -l (listen mode — that's a future feature),
 * no -u (UDP), no -e (exec). Always client-mode TCP.
 *
 * Implementation:
 *   1. Resolve the host to a 4-byte IP via DNS (or parse dotted-quad).
 *   2. Open a socket, sys_connect.
 *   3. Loop forking the work: a child copies stdin -> socket; the
 *      parent copies socket -> stdout. Either side ending closes the
 *      pair. Because we don't have non-blocking sys_read, the two-
 *      task split is the simplest way to get full duplex.
 */

#include "libuser.h"

/* Dotted-quad parser. "127.0.0.1" -> {127,0,0,1}, returns 1 on
 * success, 0 if it isn't a valid IP. Falls through to DNS. */
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
        } else {
            return 0;
        }
        s++;
    }
    if (seg != 3 || digits == 0) return 0;
    for (int i = 0; i < 4; i++) ip[i] = (unsigned char)v[i];
    return 1;
}

static int resolve(const char *host, unsigned char ip[4]) {
    if (parse_ipv4(host, ip)) return 0;
    /* Special-case "localhost" — SLIRP doesn't loop back to the
     * guest's own IP, but the kernel handles 127.0.0.1 internally. */
    if (strcmp(host, "localhost") == 0) {
        ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1;
        return 0;
    }
    return sys_dns_resolve(host, ip);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        sys_write(2, "nc: usage: nc HOST PORT\n", 24);
        return 2;
    }

    unsigned char ip[4];
    if (resolve(argv[1], ip) < 0) {
        sys_write(2, "nc: DNS resolve failed\n", 23);
        return 1;
    }
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        sys_write(2, "nc: bad port\n", 13);
        return 1;
    }

    int sk = sys_socket();
    if (sk < 0) { sys_write(2, "nc: socket failed\n", 18); return 1; }

    if (sys_connect(sk, ip, port) < 0) {
        sys_write(2, "nc: connect failed\n", 19);
        sys_close(sk);
        return 1;
    }
    /* Full duplex: child copies stdin -> socket; parent copies
     * socket -> stdout. Whoever finishes first kills the other by
     * closing the socket fd, which makes the peer's read return 0. */
    int pid = sys_fork();
    if (pid < 0) {
        sys_write(2, "nc: fork failed\n", 16);
        sys_close(sk);
        return 1;
    }

    if (pid == 0) {
        /* Child: stdin -> socket. */
        char buf[256];
        int  n;
        while ((n = sys_read(0, buf, sizeof(buf))) > 0) {
            if (sys_write(sk, buf, n) <= 0) break;
        }
        sys_close(sk);
        sys_exit(0);
    }

    /* Parent: socket -> stdout. */
    char buf[256];
    int  n;
    while ((n = sys_read(sk, buf, sizeof(buf))) > 0) {
        sys_write(1, buf, n);
    }
    sys_close(sk);

    /* Reap the child writer. If we got here because the server
     * closed the connection, the child may still be reading
     * stdin — but its first sys_write to the socket after that
     * returns -1 and the loop breaks. So we just wait, no SIGTERM
     * needed (which would risk killing a process we don't own
     * anymore if `pid` was reused). */
    int code;
    sys_wait(&code);
    return 0;
}
