/*
 * telnet — line-mode telnet client.
 *
 *   telnet HOST PORT
 *
 * Same general shape as nc, with two differences:
 *   - Strips Telnet IAC commands from the inbound stream so a
 *     server's option negotiation doesn't garble the displayed
 *     output. We refuse all options (IAC WONT / DONT) when the
 *     server asks via IAC DO / WILL.
 *   - Echoes a small banner on connect.
 *
 * RFC 854. We handle:
 *   IAC IAC      -> literal 0xFF byte (rare in practice)
 *   IAC DO/WILL  -> reply IAC WONT/DONT to refuse the option
 *   IAC DONT/WONT-> reply IAC WONT/DONT (acknowledge)
 *   IAC SB ... IAC SE  -> swallow the subnegotiation
 * Anything else after IAC is consumed as a 2-byte command.
 */

#include "libuser.h"

#define IAC      255
#define DONT     254
#define DO       253
#define WONT     252
#define WILL     251
#define SB       250
#define SE       240

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
            seg++; if (seg > 3) return 0;
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

/* IAC parser state. */
enum { TS_DATA, TS_IAC, TS_OPT, TS_SB };

int main(int argc, char **argv) {
    if (argc < 3) {
        sys_write(2, "telnet: usage: telnet HOST PORT\n", 32);
        return 2;
    }

    unsigned char ip[4];
    if (resolve(argv[1], ip) < 0) {
        sys_write(2, "telnet: DNS resolve failed\n", 27);
        return 1;
    }
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        sys_write(2, "telnet: bad port\n", 17);
        return 1;
    }

    int sk = sys_socket();
    if (sk < 0) { sys_write(2, "telnet: socket failed\n", 22); return 1; }
    if (sys_connect(sk, ip, port) < 0) {
        sys_write(2, "telnet: connect failed\n", 23);
        sys_close(sk);
        return 1;
    }

    printf("telnet: connected to %s:%d\n", argv[1], port);

    int pid = sys_fork();
    if (pid < 0) {
        sys_write(2, "telnet: fork failed\n", 20);
        sys_close(sk);
        return 1;
    }

    if (pid == 0) {
        /* Child: stdin -> socket. Line-mode by default in the kernel
         * tty layer, so each newline-terminated line gets shipped. */
        char buf[256];
        int  n;
        while ((n = sys_read(0, buf, sizeof(buf))) > 0) {
            if (sys_write(sk, buf, n) <= 0) break;
        }
        sys_close(sk);
        sys_exit(0);
    }

    /* Parent: socket -> stdout, stripping/refusing IAC commands. */
    int   ts = TS_DATA;
    int   pending_verb = 0;        /* DO/DONT/WILL/WONT pending */
    char  rbuf[256];
    int   n;
    char  out[256];
    int   oi;

    while ((n = sys_read(sk, rbuf, sizeof(rbuf))) > 0) {
        oi = 0;
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)rbuf[i];
            switch (ts) {
                case TS_DATA:
                    if (c == IAC) ts = TS_IAC;
                    else          out[oi++] = (char)c;
                    break;
                case TS_IAC:
                    if (c == IAC) {
                        out[oi++] = (char)IAC;
                        ts = TS_DATA;
                    } else if (c == DO || c == DONT || c == WILL || c == WONT) {
                        pending_verb = c;
                        ts = TS_OPT;
                    } else if (c == SB) {
                        ts = TS_SB;
                    } else {
                        ts = TS_DATA;
                    }
                    break;
                case TS_OPT: {
                    /* We refuse every option. Reply pattern:
                     *   server: IAC DO   X   -> we reply: IAC WONT X
                     *   server: IAC WILL X   -> we reply: IAC DONT X
                     *   server: IAC DONT/WONT X -> mirror back. */
                    unsigned char reply[3];
                    reply[0] = IAC;
                    if      (pending_verb == DO)   reply[1] = WONT;
                    else if (pending_verb == WILL) reply[1] = DONT;
                    else                           reply[1] = (unsigned char)pending_verb;
                    reply[2] = c;
                    sys_write(sk, reply, 3);
                    ts = TS_DATA;
                    break;
                }
                case TS_SB:
                    /* Swallow subnegotiation until IAC SE. */
                    if (c == IAC) ts = TS_IAC;   /* IAC SE re-enters here */
                    /* else swallow */
                    break;
            }
            if (oi == sizeof(out)) {
                sys_write(1, out, oi);
                oi = 0;
            }
        }
        if (oi > 0) sys_write(1, out, oi);
    }
    sys_close(sk);

    int code;
    sys_kill(pid, SIGTERM);
    sys_wait(&code);
    puts("telnet: connection closed.\n");
    return 0;
}
