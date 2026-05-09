/*
 * irc — minimal RFC 1459 client.
 *
 *   irc HOST PORT NICK [CHANNEL]
 *
 * Connects, sends NICK + USER, joins the channel, and then enters a
 * full-duplex relay where:
 *   - inbound lines are pretty-printed:
 *       <alice> hello       (PRIVMSG)
 *       *** alice JOIN #foo (JOIN)
 *       *** ircd 001 ...    (numeric reply)
 *   - stdin lines starting with `/` are commands:
 *       /quit                  -> QUIT
 *       /msg #foo hello        -> PRIVMSG #foo :hello
 *       /me waves              -> CTCP-style ACTION
 *   - other stdin lines are PRIVMSG'd to the joined channel.
 *
 * Same fork-for-duplex shape as nc/telnet: child copies stdin to
 * socket (with light command translation), parent reads from
 * socket and pretty-prints.
 */

#include "libuser.h"

#define LINE_MAX 512

static int parse_ipv4(const char *s, unsigned char ip[4]) {
    int v[4] = {0,0,0,0};
    int seg = 0, digits = 0;
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

/* Send a NUL-terminated string, appending \r\n. */
static void send_line(int sk, const char *s) {
    int len = (int)strlen(s);
    sys_write(sk, s, len);
    sys_write(sk, "\r\n", 2);
}

/* Pretty-print one received IRC line. Recognizes:
 *   :nick PRIVMSG target :text   ->  <nick> text
 *   :nick JOIN :#chan            ->  *** nick joined #chan
 *   :ircd NNN ... :text          ->  *** ircd NNN text  (numeric)
 *   PING :token                   ->  (handled invisibly: send PONG)
 * Everything else echoed verbatim.
 */
static void render(int sk, char *line) {
    /* Cut at \r or \n. */
    for (int i = 0; line[i]; i++) {
        if (line[i] == '\r' || line[i] == '\n') { line[i] = 0; break; }
    }
    if (!*line) return;

    /* Handle PING transparently. */
    if (strncmp(line, "PING ", 5) == 0) {
        char pong[80];
        const char *p = line + 5;
        if (*p == ':') p++;
        int o = 0;
        const char *pre = "PONG :";
        while (*pre) pong[o++] = *pre++;
        while (*p && o < (int)sizeof(pong) - 1) pong[o++] = *p++;
        pong[o] = 0;
        send_line(sk, pong);
        return;
    }

    /* Optional :prefix at the start. */
    char nick[64]; nick[0] = 0;
    char *p = line;
    if (*p == ':') {
        p++;
        int ni = 0;
        while (*p && *p != '!' && *p != ' ' && ni < 63) nick[ni++] = *p++;
        nick[ni] = 0;
        /* Skip user/host portion. */
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }

    /* Command. */
    char cmd[16]; int ci = 0;
    while (*p && *p != ' ' && ci < 15) cmd[ci++] = *p++;
    cmd[ci] = 0;
    while (*p == ' ') p++;

    if (strcmp(cmd, "PRIVMSG") == 0) {
        /* target then trail. */
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (*p == ':') p++;
        printf("<%s> ", nick[0] ? nick : "?");
        sys_write(1, p, (int)strlen(p));
        sys_write(1, "\n", 1);
    } else if (strcmp(cmd, "JOIN") == 0) {
        if (*p == ':') p++;
        printf("*** %s joined %s\n", nick[0] ? nick : "?", p);
    } else if (cmd[0] >= '0' && cmd[0] <= '9') {
        /* Numeric reply: "*** ircd <code> <rest>" */
        printf("*** %s %s ", nick[0] ? nick : "?", cmd);
        sys_write(1, p, (int)strlen(p));
        sys_write(1, "\n", 1);
    } else {
        /* Fallback: echo. */
        printf("=== ");
        sys_write(1, line, (int)strlen(line));
        sys_write(1, "\n", 1);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        sys_write(2,
            "irc: usage: irc HOST PORT NICK [CHANNEL]\n", 41);
        return 2;
    }

    unsigned char ip[4];
    if (resolve(argv[1], ip) < 0) {
        sys_write(2, "irc: DNS resolve failed\n", 24);
        return 1;
    }
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        sys_write(2, "irc: bad port\n", 14);
        return 1;
    }

    const char *nick = argv[3];
    const char *chan = (argc >= 5) ? argv[4] : "#demo";

    int sk = sys_socket();
    if (sk < 0) { sys_write(2, "irc: socket failed\n", 19); return 1; }
    if (sys_connect(sk, ip, port) < 0) {
        sys_write(2, "irc: connect failed\n", 20);
        sys_close(sk);
        return 1;
    }

    /* NICK + USER + JOIN. */
    char nickline[64], userline[128], joinline[64];
    int  o;

    o = 0;
    const char *pn = "NICK ";
    while (*pn) nickline[o++] = *pn++;
    while (*nick) nickline[o++] = *nick++;
    nickline[o] = 0;
    nick = argv[3];           /* restore for later use */

    o = 0;
    const char *pu1 = "USER "; while (*pu1) userline[o++] = *pu1++;
    const char *u = nick;     while (*u)   userline[o++] = *u++;
    const char *pu2 = " 0 * :"; while (*pu2) userline[o++] = *pu2++;
    u = nick;                 while (*u)   userline[o++] = *u++;
    userline[o] = 0;

    o = 0;
    const char *pj = "JOIN "; while (*pj) joinline[o++] = *pj++;
    const char *c = chan;     while (*c)  joinline[o++] = *c++;
    joinline[o] = 0;

    send_line(sk, nickline);
    send_line(sk, userline);
    send_line(sk, joinline);

    int pid = sys_fork();
    if (pid < 0) {
        sys_write(2, "irc: fork failed\n", 17);
        sys_close(sk);
        return 1;
    }

    if (pid == 0) {
        /* Child: stdin -> socket with command translation. */
        char input[LINE_MAX];
        int  n;
        while ((n = sys_read_line(input, sizeof(input))) > 0) {
            /* sys_read_line returns a NUL-terminated string of
             * length n (excluding NUL). */
            if (input[0] == '/') {
                if (strncmp(input, "/quit", 5) == 0) {
                    send_line(sk, "QUIT :bye");
                    break;
                }
                if (strncmp(input, "/me ", 4) == 0) {
                    /* CTCP ACTION: PRIVMSG #chan :\x01ACTION text\x01 */
                    char out[LINE_MAX];
                    int  oo = 0;
                    const char *p1 = "PRIVMSG ";
                    while (*p1) out[oo++] = *p1++;
                    const char *cc = chan; while (*cc) out[oo++] = *cc++;
                    out[oo++] = ' '; out[oo++] = ':'; out[oo++] = 0x01;
                    const char *p2 = "ACTION "; while (*p2) out[oo++] = *p2++;
                    const char *body = input + 4;
                    while (*body) out[oo++] = *body++;
                    out[oo++] = 0x01;
                    out[oo] = 0;
                    send_line(sk, out);
                    continue;
                }
                if (strncmp(input, "/msg ", 5) == 0) {
                    /* /msg target text -> PRIVMSG target :text */
                    const char *body = input + 5;
                    char target[32]; int ti = 0;
                    while (*body && *body != ' ' && ti < 31) target[ti++] = *body++;
                    target[ti] = 0;
                    while (*body == ' ') body++;
                    char out[LINE_MAX]; int oo = 0;
                    const char *p1 = "PRIVMSG "; while (*p1) out[oo++] = *p1++;
                    for (int i = 0; target[i]; i++) out[oo++] = target[i];
                    out[oo++] = ' '; out[oo++] = ':';
                    while (*body) out[oo++] = *body++;
                    out[oo] = 0;
                    send_line(sk, out);
                    continue;
                }
                /* Unknown / -> drop. */
                continue;
            }
            /* Bare text: PRIVMSG to the joined channel. */
            char out[LINE_MAX];
            int  oo = 0;
            const char *p1 = "PRIVMSG "; while (*p1) out[oo++] = *p1++;
            const char *cc = chan;       while (*cc) out[oo++] = *cc++;
            out[oo++] = ' '; out[oo++] = ':';
            for (int i = 0; i < n && oo < LINE_MAX - 1; i++) out[oo++] = input[i];
            out[oo] = 0;
            send_line(sk, out);
        }
        sys_close(sk);
        sys_exit(0);
    }

    /* Parent: socket -> stdout, pretty-printed. */
    char rbuf[256];
    char line[LINE_MAX];
    int  li = 0;
    int  n;
    while ((n = sys_read(sk, rbuf, sizeof(rbuf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (li < LINE_MAX - 1) line[li++] = rbuf[i];
            if (rbuf[i] == '\n' || li == LINE_MAX - 1) {
                line[li] = 0;
                render(sk, line);
                li = 0;
            }
        }
    }
    sys_close(sk);

    int code;
    sys_kill(pid, SIGTERM);
    sys_wait(&code);
    puts("irc: disconnected\n");
    return 0;
}
