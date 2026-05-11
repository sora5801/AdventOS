/*
 * ssh — TLS-over-TCP remote shell client for AdventOS.
 *
 * Mirror of sshd.c. Opens a TCP socket to the target, runs a TLS 1.3
 * client handshake (cert flow, no validation — same `-k` semantics
 * as httpsget), then shuttles bytes:
 *
 *   server → 0x01 sentinel    →   "your turn"
 *   read local stdin line     →   send over TLS
 *   server processes          →   bytes flow back
 *   loop
 *
 * Usage:
 *   ssh <ip>[:port]      port defaults to 2222
 *
 * Example interactive session:
 *
 *   advent$ ssh 127.0.0.1
 *   AdventOS sshd over TLS 1.3 (ECDSA-P256)
 *   login: guest
 *   password: guest
 *   Welcome to AdventOS over TLS.  user=guest  shell=sh.elf -c
 *   advent-ssh$ id
 *   uid=1000 gid=1000 ...
 *   advent-ssh$ exit
 *   bye
 *
 * No SSH-2 protocol — see sshd.c's header for the framing details.
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/tls.h"

#define DEFAULT_PORT  2222
#define LINE_MAX      256

/* "1.2.3.4" / "1.2.3.4:NNNN" → ip[4] + port. Returns 0 on success.
 * No DNS — the kernel's resolver works but this is a demo client and
 * keeping the codepath dotted-quad-only avoids a DNS dependency in
 * the loopback selftest. */
static int parse_ip_port(const char *s, unsigned char ip[4], int *port_out) {
    int dots = 0;
    int val  = 0;
    int seen = 0;
    int idx  = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            seen = 1;
        } else if (*s == '.') {
            if (!seen || val > 255 || idx >= 3) return -1;
            ip[idx++] = (unsigned char)val;
            val = 0; seen = 0; dots++;
        } else if (*s == ':') {
            break;
        } else {
            return -1;
        }
        s++;
    }
    if (dots != 3 || !seen || val > 255) return -1;
    ip[3] = (unsigned char)val;

    if (*s == ':') {
        s++;
        int p = 0;
        while (*s >= '0' && *s <= '9') { p = p * 10 + (*s - '0'); s++; }
        if (p <= 0 || p > 65535) return -1;
        *port_out = p;
    } else {
        *port_out = DEFAULT_PORT;
    }
    return 0;
}

/* tls_send for a NUL-terminated string. */
static int send_str(struct tls_conn *t, const char *s) {
    int n = 0; while (s[n]) n++;
    return tls_send(t, s, n);
}

/* Drain TLS bytes to stdout until we either see the 0x01 ready
 * sentinel (server's "your turn") or the connection closes. Returns:
 *   1  saw the sentinel — caller should read stdin next
 *   0  EOF / connection closed cleanly
 *  -1  error
 *
 * The sentinel itself is consumed but not printed. Any bytes after it
 * in the same record are pushed to stdout — they're regular output
 * for the next interaction (rare in practice). */
static int drain_until_ready(struct tls_conn *t) {
    char buf[1024];
    for (;;) {
        int n = tls_recv(t, buf, sizeof(buf));
        if (n <= 0) return (n == 0) ? 0 : -1;
        int sentinel = -1;
        for (int i = 0; i < n; i++) {
            if (buf[i] == 0x01) { sentinel = i; break; }
        }
        int print_end = (sentinel < 0) ? n : sentinel;
        if (print_end > 0) sys_write(1, buf, print_end);
        if (sentinel >= 0) {
            if (sentinel + 1 < n) sys_write(1, buf + sentinel + 1, n - sentinel - 1);
            return 1;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: ssh <ip>[:port]\n");
        return 1;
    }

    unsigned char ip[4];
    int port;
    if (parse_ip_port(argv[1], ip, &port) < 0) {
        puts("ssh: bad host:port — try 127.0.0.1 or 10.0.2.2:2222\n");
        return 1;
    }
    printf("ssh: connecting to %d.%d.%d.%d:%d ...\n",
           ip[0], ip[1], ip[2], ip[3], port);

    int sk = sys_socket();
    if (sk < 0) { puts("ssh: socket() failed\n"); return 1; }
    if (sys_connect(sk, ip, port) < 0) {
        puts("ssh: connect failed (is sshd running?)\n");
        sys_close(sk);
        return 1;
    }
    puts("ssh: TCP connected; starting TLS handshake\n");

    struct tls_conn t;
    t.server_name = 0;       /* IP literal, no SNI */
    if (tls_client_handshake_cert(&t, sk) != 0) {
        puts("ssh: TLS handshake failed\n");
        sys_close(sk);
        return 1;
    }
    puts("ssh: TLS 1.3 up; entering interactive session\n");

    /* The conversation: server sends a prompt + 0x01, we read a line
     * from stdin and send it back, server runs and replies, repeat.
     * Loop ends on connection close or local stdin EOF. */
    for (;;) {
        int r = drain_until_ready(&t);
        if (r <= 0) break;

        char line[LINE_MAX];
        int n = sys_read_line(line, sizeof(line));
        if (n <= 0) {
            /* Stdin EOF — send an "exit" line and bail. */
            send_str(&t, "exit\n");
            /* One last drain so the server's "bye\n" gets to stdout. */
            char buf[256];
            for (;;) {
                int g = tls_recv(&t, buf, sizeof(buf));
                if (g <= 0) break;
                /* Skip any trailing 0x01 — we're not going to read more. */
                for (int i = 0; i < g; i++) if (buf[i] != 0x01) sys_write(1, &buf[i], 1);
            }
            break;
        }

        /* sys_read_line returns the line WITH its trailing newline. */
        if (line[n - 1] != '\n') line[n++] = '\n';
        tls_send(&t, line, n);

        /* If the user just typed `exit`, drain the server's final
         * "bye\n" + RST and exit cleanly without another stdin read. */
        if (line[0] == 'e' && line[1] == 'x' && line[2] == 'i' &&
            line[3] == 't' && (line[4] == '\n' || line[4] == 0)) {
            char buf[256];
            for (;;) {
                int g = tls_recv(&t, buf, sizeof(buf));
                if (g <= 0) break;
                for (int i = 0; i < g; i++) if (buf[i] != 0x01) sys_write(1, &buf[i], 1);
            }
            break;
        }
    }

    puts("\nssh: connection closed.\n");
    sys_close(sk);
    return 0;
}
