/*
 * Userspace HTTP/1.0 server.
 *
 *   socket() bind() listen(BACKLOG) accept() -> read/write/close
 *
 * Listen with a backlog of 8: up to 8 SYNs can complete the 3-way
 * handshake while we're in the middle of serving the current one,
 * and each gets pushed onto the listener's accept queue (session 30).
 * The server itself processes connections sequentially — pull one
 * off the queue, write the canned response, close, repeat.
 *
 * Concurrent CLIENTS work cleanly because the queue holds them
 * during the time we're blocked. Concurrent SERVING (fork-per-
 * connection) is a known followup: the existing test infrastructure
 * uses internal-fork pipelines (nc + cat | ... ) that interact
 * subtly with httpd's own forking. Sequential serving is plenty
 * for the demo and doesn't drop SYNs.
 */

#include "libuser.h"

static const char *g_response =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Hello from a USERSPACE HTTP server!\n"
    "\n"
    "This page was served by user/httpd.c, which runs in ring 3.\n"
    "It calls socket()/bind()/listen()/accept() through INT 0x80,\n"
    "and read/write/close on the returned fd flow through the same\n"
    "task_fd[] table that backs file I/O — TCP sockets are just\n"
    "another fd kind on the kernel side.\n";

static void handle_client(int conn) {
    char buf[1024];
    int  n = sys_read(conn, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        sys_write(1, "httpd: ", 7);
        int i;
        for (i = 0; i < n && buf[i] != '\r' && buf[i] != '\n'; i++) {}
        sys_write(1, buf, i);
        sys_write(1, "\n", 1);
    }
    int rlen = (int)strlen(g_response);
    sys_write(conn, g_response, rlen);
    sys_close(conn);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int s = sys_socket();
    if (s < 0)                       { puts("httpd: socket() failed\n"); return 1; }
    if (sys_bind(s, 80) < 0)         { puts("httpd: bind(80) failed\n"); return 1; }
    /* listen with a backlog of 8 — concurrent clients can all complete
     * the 3-way handshake while the parent is mid-fork. */
    if (sys_listen(s, 8) < 0)        { puts("httpd: listen() failed\n"); return 1; }

    /* listening — silent */

    for (;;) {
        int conn = sys_accept(s);
        if (conn < 0) continue;
        handle_client(conn);
    }
    return 0;
}
