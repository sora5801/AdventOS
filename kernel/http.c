#include "http.h"
#include "tcp.h"
#include "string.h"
#include "kprintf.h"

static const char *g_response =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Hello from AdventOS!\n"
    "Userspace shell + minimal TCP/IP/HTTP working.\n"
    "Path: served by kernel/http.c after a real 3-way handshake.\n";

static int g_handled_this_conn;

static void http_on_recv(struct tcb *t, const char *data, int len) {
    if (g_handled_this_conn) return;     /* one response per connection */
    g_handled_this_conn = 1;

    /* Echo the request line (or its prefix) for the demo. */
    int show = len < 60 ? len : 60;
    kputs("httpd: ");
    for (int i = 0; i < show && data[i] != '\r' && data[i] != '\n'; i++) {
        kputc(data[i]);
    }
    kputc('\n');

    /* Send the canned response and start the close. */
    int rlen = (int)strlen(g_response);
    tcp_send(t, g_response, rlen);
    tcp_close(t);
}

static void http_on_close(struct tcb *t) {
    (void)t;
    g_handled_this_conn = 0;
}

void http_init(void) {
    g_handled_this_conn = 0;
    tcp_listen(80, http_on_recv, http_on_close);
}
