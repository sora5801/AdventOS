/*
 * Userspace HTTP/1.0 server. Replaces the kernel-resident http.c;
 * everything below runs in ring 3 against the new sockets API:
 *   socket() bind() listen() accept() -> read() write() close()
 *
 * Same wire behavior as before: serves a canned 200 OK on every
 * connection, then closes. curl localhost:8080 still works.
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

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int s = sys_socket();
    if (s < 0)                      { puts("httpd: socket() failed\n"); return 1; }
    if (sys_bind(s, 80) < 0)        { puts("httpd: bind(80) failed\n"); return 1; }
    if (sys_listen(s, 1) < 0)       { puts("httpd: listen() failed\n"); return 1; }

    puts("httpd: listening on port 80 (userspace)\n");

    for (;;) {
        int conn = sys_accept(s);
        if (conn < 0) continue;

        /* Read up to one buffer's worth of request bytes. We don't
         * actually parse anything — every request gets the same
         * canned response. */
        char buf[1024];
        int  n = sys_read(conn, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            /* Print the request line for the demo. */
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
    return 0;
}
