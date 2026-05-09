/*
 * ircd — single-channel IRC server. Just enough to answer the
 * client's NICK / USER / JOIN / PRIVMSG sequence and echo PRIVMSGs
 * back so the demo client has something real to interact with.
 *
 *   ircd PORT
 *
 * Protocol (RFC 1459 subset):
 *   <- NICK alice
 *   <- USER alice 0 * :Alice
 *   -> :ircd 001 alice :Welcome
 *   <- JOIN #demo
 *   -> :alice JOIN :#demo
 *   -> :ircd 353 alice = #demo :alice
 *   -> :ircd 366 alice #demo :End of /NAMES list.
 *   <- PRIVMSG #demo :hello
 *   -> :alice PRIVMSG #demo :hello   (echo back so the user sees it)
 *
 * Single-client at a time. The kernel's TCP listener accept-loop
 * is fronted by sock layer so that's structurally fine.
 */

#include "libuser.h"

#define LINE_MAX 512

static char g_nick[32] = "guest";
static int  g_have_nick = 0;
static int  g_have_user = 0;

/* Send a formatted line ending with \r\n. */
static void send_line(int fd, const char *prefix,
                      const char *fields[], int nfields) {
    char buf[LINE_MAX];
    int  o = 0;
    if (prefix) {
        if (o < LINE_MAX - 1) buf[o++] = ':';
        for (int j = 0; prefix[j] && o < LINE_MAX - 1; j++) buf[o++] = prefix[j];
        if (o < LINE_MAX - 1) buf[o++] = ' ';
    }
    for (int i = 0; i < nfields; i++) {
        const char *s = fields[i];
        if (i > 0 && o < LINE_MAX - 1) buf[o++] = ' ';
        while (*s && o < LINE_MAX - 1) buf[o++] = *s++;
    }
    if (o < LINE_MAX - 1) buf[o++] = '\r';
    if (o < LINE_MAX - 1) buf[o++] = '\n';
    sys_write(fd, buf, o);
}

/* Locate the first whitespace-delimited token starting at *p. Sets
 * *p to point past the token; writes up to cap-1 bytes into out. */
static void take_token(const char **p, char *out, int cap) {
    const char *s = *p;
    while (*s == ' ') s++;
    int o = 0;
    while (*s && *s != ' ' && *s != '\r' && *s != '\n' && o < cap - 1) {
        out[o++] = *s++;
    }
    out[o] = 0;
    *p = s;
}

/* Locate the colon-prefixed trailing argument starting at *p.
 * Writes up to cap-1 bytes. Trailing arg starts after a ' :'. */
static void take_trail(const char *p, char *out, int cap) {
    while (*p == ' ') p++;
    if (*p == ':') p++;
    int o = 0;
    while (*p && *p != '\r' && *p != '\n' && o < cap - 1) out[o++] = *p++;
    out[o] = 0;
}

static void handle_line(int fd, char *line) {
    /* Cut at \r or \n. */
    for (int i = 0; line[i]; i++) {
        if (line[i] == '\r' || line[i] == '\n') { line[i] = 0; break; }
    }
    if (!*line) return;

    /* Optional server prefix — clients don't send one. */
    const char *p = line;
    if (*p == ':') {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }

    char cmd[16];
    take_token(&p, cmd, sizeof(cmd));

    if (strcmp(cmd, "NICK") == 0) {
        char nick[32];
        take_token(&p, nick, sizeof(nick));
        if (nick[0]) {
            for (int i = 0; i < (int)sizeof(g_nick) - 1; i++) {
                g_nick[i] = nick[i];
                if (!nick[i]) break;
            }
            g_nick[sizeof(g_nick) - 1] = 0;
            g_have_nick = 1;
        }
    } else if (strcmp(cmd, "USER") == 0) {
        g_have_user = 1;
    } else if (strcmp(cmd, "JOIN") == 0) {
        char chan[32];
        take_token(&p, chan, sizeof(chan));
        /* :nick JOIN :#chan */
        char nickjoin[64];
        int  o = 0;
        for (int i = 0; g_nick[i]; i++) nickjoin[o++] = g_nick[i];
        nickjoin[o] = 0;
        const char *parts[]  = { "JOIN", ":", chan };
        const char *parts2[] = { "353", g_nick, "=", chan, ":", g_nick };
        const char *parts3[] = { "366", g_nick, chan, ":End of /NAMES list." };
        send_line(fd, nickjoin, parts,  3);
        send_line(fd, "ircd",   parts2, 6);
        send_line(fd, "ircd",   parts3, 4);
    } else if (strcmp(cmd, "PRIVMSG") == 0) {
        char target[32];
        take_token(&p, target, sizeof(target));
        char text[256];
        take_trail(p, text, sizeof(text));
        /* Echo the message back as if the user said it. Since we
         * have one client, this lets the user see what they typed. */
        char nickprefix[64];
        int  o = 0;
        for (int i = 0; g_nick[i]; i++) nickprefix[o++] = g_nick[i];
        nickprefix[o] = 0;
        const char *parts[] = { "PRIVMSG", target, ":", text };
        send_line(fd, nickprefix, parts, 4);
    } else if (strcmp(cmd, "PING") == 0) {
        char tok[64];
        take_token(&p, tok, sizeof(tok));
        const char *parts[] = { "PONG", "ircd", ":", tok };
        send_line(fd, "ircd", parts, 4);
    } else if (strcmp(cmd, "QUIT") == 0) {
        const char *parts[] = { "ERROR", ":Closing link" };
        send_line(fd, 0, parts, 2);
        /* Force-close the connection. We don't have shutdown(2),
         * so a half-close (client stops writing but keeps reading)
         * isn't representable: the conn fd is shared between the
         * irc client's parent + child, refcount=2, and a single
         * close from the child decrements without sending FIN.
         * Closing from the server side propagates a FIN to the
         * client and unblocks the parent's read. */
        sys_close(fd);
    }

    /* When both NICK and USER have been received, send the welcome. */
    if (g_have_nick && g_have_user) {
        const char *parts[] = { "001", g_nick, ":Welcome to AdventOS IRC" };
        send_line(fd, "ircd", parts, 3);
        g_have_user = 0;     /* one-shot */
    }
}

int main(int argc, char **argv) {
    int port = 6667;
    int how_many = 1;       /* default: handle one client and exit */
    if (argc >= 2) {
        int p = atoi(argv[1]);
        if (p > 0 && p <= 65535) port = p;
    }
    if (argc >= 3) {
        if (strcmp(argv[2], "loop") == 0) how_many = 0;   /* unlimited */
    }

    int srv = sys_socket();
    if (srv < 0) { sys_write(2, "ircd: socket\n", 13); return 1; }
    sys_bind  (srv, port);
    sys_listen(srv, 1);
    printf("ircd: listening on port %d (%s)\n",
           port, how_many ? "one-shot" : "loop");

    int handled = 0;
    while (how_many == 0 || handled < how_many) {
        int conn = sys_accept(srv);
        if (conn < 0) { sys_sleep_ms(50); continue; }

        g_have_nick = 0;
        g_have_user = 0;
        for (int i = 0; i < 32; i++) g_nick[i] = 0;
        const char *def = "guest";
        for (int i = 0; def[i]; i++) g_nick[i] = def[i];

        /* Read line-by-line. Each \r\n-terminated line goes through
         * handle_line. Buffer carries partial lines across reads. */
        char line[LINE_MAX];
        int  li = 0;
        char buf[256];
        int  n;
        while ((n = sys_read(conn, buf, sizeof(buf))) > 0) {
            for (int i = 0; i < n; i++) {
                if (li < LINE_MAX - 1) line[li++] = buf[i];
                if (buf[i] == '\n' || li == LINE_MAX - 1) {
                    line[li] = 0;
                    handle_line(conn, line);
                    li = 0;
                }
            }
        }
        sys_close(conn);
        handled++;
        printf("ircd: client gone (handled %d so far)\n", handled);
    }
    sys_close(srv);
    return 0;
}
