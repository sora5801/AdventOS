#include "sock.h"
#include "tcp.h"
#include "task.h"
#include "string.h"
#include "kprintf.h"

static struct sock g_socks[SOCK_MAX];

/* ---- listener accept queue ---------------------------------------- */

static int q_empty(struct sock *s) {
    return s->pending_head == s->pending_tail;
}
static int q_full(struct sock *s) {
    /* Treat backlog as the cap; full when adding would equal tail. */
    int next = (s->pending_head + 1) % SOCK_BACKLOG_MAX;
    return next == s->pending_tail ||
           ((s->pending_head - s->pending_tail + SOCK_BACKLOG_MAX) % SOCK_BACKLOG_MAX
                >= s->backlog);
}
static void q_init(struct sock *s, int backlog) {
    if (backlog <= 0)               backlog = 1;
    if (backlog > SOCK_BACKLOG_MAX) backlog = SOCK_BACKLOG_MAX;
    s->backlog      = backlog;
    s->pending_head = 0;
    s->pending_tail = 0;
    for (int i = 0; i < SOCK_BACKLOG_MAX; i++) s->pending_conns[i] = -1;
}
static int q_push(struct sock *s, int conn_idx) {
    if (q_full(s)) return -1;
    s->pending_conns[s->pending_head] = conn_idx;
    s->pending_head = (s->pending_head + 1) % SOCK_BACKLOG_MAX;
    return 0;
}
static int q_pop(struct sock *s) {
    if (q_empty(s)) return -1;
    int v = s->pending_conns[s->pending_tail];
    s->pending_conns[s->pending_tail] = -1;
    s->pending_tail = (s->pending_tail + 1) % SOCK_BACKLOG_MAX;
    return v;
}

void sock_init(void) {
    for (int i = 0; i < SOCK_MAX; i++) {
        memset(&g_socks[i], 0, sizeof(g_socks[i]));
        g_socks[i].state    = SOCK_FREE;
        q_init(&g_socks[i], 0);
    }
}

int sock_create(void) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].state == SOCK_FREE) {
            memset(&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].state    = SOCK_NEW;
            g_socks[i].refcount = 1;
            g_socks[i].tcb      = 0;
            q_init(&g_socks[i], 0);
            return i;
        }
    }
    return -1;
}

void sock_inc_ref(int idx) {
    if (idx < 0 || idx >= SOCK_MAX) return;
    if (g_socks[idx].state == SOCK_FREE) return;
    g_socks[idx].refcount++;
}

int sock_bind(int idx, uint16_t port) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    if (g_socks[idx].state != SOCK_NEW) return -1;
    g_socks[idx].port = port;
    return 0;
}

/* Find the sock owning `t` via t->user_data. Returns -1 if none. */
static int sock_for_tcb(struct tcb *t) {
    if (!t || !t->user_data) return -1;
    int idx = (int)(uintptr_t)t->user_data - 1;     /* +1 encoding */
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    if (g_socks[idx].state == SOCK_FREE) return -1;
    return idx;
}

/* ---- TCP callbacks ------------------------------------------------- */

/* Fires (in IRQ context) the moment a TCB transitions to ESTABLISHED.
 * For a connected sock this just flips its state. For a listener-
 * spawned conn TCB, we synthesize a fresh conn sock and stash it on
 * the listener so sock_accept() can pull it back out. */
static void on_connect(struct tcb *t) {
    int sidx = sock_for_tcb(t);
    if (sidx < 0) return;
    struct sock *s = &g_socks[sidx];

    /* Active connect path: the calling task is in sock_connect, sock
     * is CONNECTING. We can't compare s->tcb == t because sock_connect
     * sets s->tcb AFTER tcp_connect returns — but the loopback fires
     * on_connect DURING tcp_connect, before s->tcb has been written.
     * State alone is enough to discriminate from the listener path. */
    if (s->state == SOCK_CONNECTING) {
        s->state = SOCK_CONNECTED;
        s->tcb   = t;       /* publish in case sock_connect hasn't yet */
        return;
    }

    /* Listener path: this conn TCB was spawned by an inbound SYN
     * landing on a listener. Create a fresh conn sock for it. */
    struct sock *l = s;
    if (l->state != SOCK_LISTEN) return;

    /* Refuse if our backlog ring is full. The TCB has already
     * been allocated; we'd ideally tear it down here so the
     * peer sees RST, but for now leak — the kernel reaper will
     * release it when its state machine progresses. */
    if (q_full(l)) return;

    int conn = -1;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].state == SOCK_FREE) { conn = i; break; }
    }
    if (conn < 0) return;        /* no free slot — drop the conn */

    memset(&g_socks[conn], 0, sizeof(g_socks[conn]));
    g_socks[conn].state        = SOCK_CONNECTED;
    g_socks[conn].refcount     = 1;
    g_socks[conn].tcb          = t;
    g_socks[conn].peer_closed  = 0;
    q_init(&g_socks[conn], 0);

    /* Re-target the conn TCB's user_data at the new conn sock so
     * subsequent on_recv / on_close land on the right ring. */
    t->user_data = (void *)(uintptr_t)(conn + 1);

    q_push(l, conn);
}

/* Fires per data segment (IRQ context). Pushes bytes into the conn
 * sock's RX ring; drops the rest on overflow. */
static void on_recv(struct tcb *t, const char *data, int len) {
    int sidx = sock_for_tcb(t);
    if (sidx < 0) return;
    struct sock *s = &g_socks[sidx];

    for (int i = 0; i < len; i++) {
        uint32_t next = (s->rx_head + 1) % SOCK_RX_BUF;
        if (next == s->rx_tail) break;          /* ring full → drop */
        s->rx_buf[s->rx_head] = (uint8_t)data[i];
        s->rx_head = next;
    }
}

static void on_close(struct tcb *t) {
    int sidx = sock_for_tcb(t);
    if (sidx < 0) return;
    g_socks[sidx].peer_closed = 1;
}

/* ---- Public ops ---------------------------------------------------- */

int sock_listen(int idx, int backlog) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    if (g_socks[idx].state != SOCK_NEW) return -1;
    g_socks[idx].state = SOCK_LISTEN;
    q_init(&g_socks[idx], backlog);
    /* user_data is sock_idx + 1 so 0 is reserved for "no callback". */
    void *ud = (void *)(uintptr_t)(idx + 1);
    g_socks[idx].tcb = tcp_listen(g_socks[idx].port,
                                  on_connect, on_recv, on_close, ud);
    if (!g_socks[idx].tcb) {
        g_socks[idx].state = SOCK_NEW;
        return -1;
    }
    return 0;
}

int sock_accept(int idx) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    if (g_socks[idx].state != SOCK_LISTEN) return -1;

    /* Block until something's queued. Multiple SYNs can land while
     * we're blocked here; each one pushes a fresh conn-sock into
     * the listener's pending ring (capped at backlog). */
    while (q_empty(&g_socks[idx])) {
        task_yield();
    }
    return q_pop(&g_socks[idx]);
}

int sock_connect(int idx, const uint8_t dst[4], uint16_t port) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    if (g_socks[idx].state != SOCK_NEW) return -1;
    if (!dst) return -1;

    struct ip_addr remote;
    for (int i = 0; i < 4; i++) remote.b[i] = dst[i];

    g_socks[idx].state        = SOCK_CONNECTING;
    g_socks[idx].peer_closed  = 0;
    g_socks[idx].rx_head      = 0;
    g_socks[idx].rx_tail      = 0;
    q_init(&g_socks[idx], 0);

    void *ud = (void *)(uintptr_t)(idx + 1);
    struct tcb *t = tcp_connect(&remote, port,
                                on_connect, on_recv, on_close, ud);
    if (!t) {
        g_socks[idx].state = SOCK_NEW;
        return -1;
    }
    g_socks[idx].tcb = t;

    /* Block until ESTABLISHED, dropped, or timed out. We give it
     * ~5 seconds at 10ms/yield resolution before giving up. */
    int spins = 0;
    while (g_socks[idx].state == SOCK_CONNECTING && spins < 500) {
        task_yield();
        spins++;
        /* If the TCB got released (peer RST etc.) we won't see
         * a state change from on_connect — detect via tcb state. */
        if (tcp_state_of(t) == TCP_CLOSED) break;
    }
    if (g_socks[idx].state != SOCK_CONNECTED) {
        g_socks[idx].state = SOCK_NEW;
        g_socks[idx].tcb   = 0;
        return -1;
    }
    return 0;
}

int sock_read(int idx, void *buf, int n) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    struct sock *s = &g_socks[idx];
    if (s->state != SOCK_CONNECTED && s->state != SOCK_CLOSED) return -1;

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
    if (!s->tcb) return -1;
    return tcp_send(s->tcb, buf, n);
}

int sock_close(int idx) {
    if (idx < 0 || idx >= SOCK_MAX) return -1;
    struct sock *s = &g_socks[idx];
    if (s->state == SOCK_FREE) return -1;

    /* Refcount: an fd close decrements; only when it hits zero do
     * we tear down the underlying TCB and free the slot. fork()
     * inheritance bumps the count via sock_inc_ref. Without this
     * a forked child closing its copy of the fd would yank the
     * socket out from under the parent — exactly what nc/wget/irc
     * need to avoid since they fork for full duplex. */
    if (s->refcount > 1) {
        s->refcount--;
        return 0;
    }
    if (s->tcb) {
        tcp_close(s->tcb);
        /* tcp_close may have released the tcb; don't deref. */
        s->tcb = 0;
    }
    s->state    = SOCK_FREE;
    s->refcount = 0;
    q_init(s, 0);
    return 0;
}
