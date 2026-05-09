#include "sock.h"
#include "tcp.h"
#include "task.h"
#include "string.h"
#include "kprintf.h"

static struct sock g_socks[SOCK_MAX];

/* Indices of "the" listener and "the" connection. Only one of each
 * exists at a time given the single-TCB underlying TCP. */
static int g_listen_idx = -1;
static int g_conn_idx   = -1;

void sock_init(void) {
    for (int i = 0; i < SOCK_MAX; i++) g_socks[i].state = SOCK_FREE;
    g_listen_idx = -1;
    g_conn_idx   = -1;
}

int sock_create(void) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].state == SOCK_FREE) {
            memset(&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].state = SOCK_NEW;
            g_socks[i].pending_conn = -1;
            return i;
        }
    }
    return -1;
}

int sock_bind(int idx, uint16_t port) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    if (g_socks[idx].state != SOCK_NEW) return -1;
    g_socks[idx].port = port;
    return 0;
}

/* ---- TCP callbacks ------------------------------------------------- */

/* Fires (in IRQ context) the moment SYN_RCVD -> ESTABLISHED. We
 * synthesize a fresh connection sock and stash its index on the
 * listener; sock_accept() will pull it back out. */
static void on_connect(struct tcb *t) {
    (void)t;
    if (g_listen_idx < 0) return;
    struct sock *l = &g_socks[g_listen_idx];
    if (l->pending_conn >= 0) return;          /* listener already has one queued */

    int conn = -1;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].state == SOCK_FREE) { conn = i; break; }
    }
    if (conn < 0) return;                       /* no free slot, drop */

    memset(&g_socks[conn], 0, sizeof(g_socks[conn]));
    g_socks[conn].state         = SOCK_CONNECTED;
    g_socks[conn].pending_conn  = -1;
    g_socks[conn].peer_closed   = 0;

    l->pending_conn = conn;
    g_conn_idx      = conn;
}

/* Fires per data segment (IRQ context). Pushes bytes into the conn
 * sock's RX ring; drops the rest on overflow. */
static void on_recv(struct tcb *t, const char *data, int len) {
    (void)t;
    if (g_conn_idx < 0) return;
    struct sock *s = &g_socks[g_conn_idx];

    for (int i = 0; i < len; i++) {
        uint32_t next = (s->rx_head + 1) % SOCK_RX_BUF;
        if (next == s->rx_tail) break;          /* ring full → drop */
        s->rx_buf[s->rx_head] = (uint8_t)data[i];
        s->rx_head = next;
    }
}

static void on_close(struct tcb *t) {
    (void)t;
    if (g_conn_idx < 0) return;
    g_socks[g_conn_idx].peer_closed = 1;
}

/* ---- Public ops ---------------------------------------------------- */

int sock_listen(int idx) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    if (g_socks[idx].state != SOCK_NEW) return -1;
    g_socks[idx].state        = SOCK_LISTEN;
    g_socks[idx].pending_conn = -1;
    g_listen_idx              = idx;
    return tcp_listen(g_socks[idx].port, on_connect, on_recv, on_close);
}

int sock_accept(int idx) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    if (g_socks[idx].state != SOCK_LISTEN) return -1;

    /* Block until a connection is queued (set by on_connect). The
     * TCP rx path is IRQ-driven so we don't have to do anything else
     * here — just yield until our flag flips. */
    while (g_socks[idx].pending_conn < 0) {
        task_yield();
    }
    int conn = g_socks[idx].pending_conn;
    g_socks[idx].pending_conn = -1;
    return conn;
}

int sock_read(int idx, void *buf, int n) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    struct sock *s = &g_socks[idx];
    if (s->state != SOCK_CONNECTED) return -1;

    /* Block until some data has arrived OR the peer has closed and
     * the buffer is drained. */
    while (s->rx_head == s->rx_tail && !s->peer_closed) {
        task_yield();
    }

    int copied = 0;
    char *out = (char *)buf;
    while (copied < n && s->rx_tail != s->rx_head) {
        out[copied++] = (char)s->rx_buf[s->rx_tail];
        s->rx_tail = (s->rx_tail + 1) % SOCK_RX_BUF;
    }
    return copied;     /* 0 = EOF (peer closed and ring drained) */
}

int sock_write(int idx, const void *buf, int n) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    struct sock *s = &g_socks[idx];
    if (s->state != SOCK_CONNECTED) return -1;
    return tcp_send_active(buf, n);
}

int sock_close(int idx) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    struct sock *s = &g_socks[idx];

    if (s->state == SOCK_CONNECTED) {
        tcp_close_active();
        if (idx == g_conn_idx) g_conn_idx = -1;
    } else if (s->state == SOCK_LISTEN) {
        if (idx == g_listen_idx) g_listen_idx = -1;
        /* TCP listener stays alive (back_to_listen re-arms it); we
         * just stop tracking it here. The next handshake will fire
         * on_connect with g_listen_idx == -1 and be silently dropped. */
    }
    s->state = SOCK_FREE;
    return 0;
}
