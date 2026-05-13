#ifndef ADVENTOS_SOCK_H
#define ADVENTOS_SOCK_H

#include "../include/types.h"
#include "ip.h"

/*
 * Sockets layer. Wraps tcp.c's multi-TCB stack with a per-fd data
 * structure and a RX ring buffer, exposing a blocking accept /
 * read / write / close API suitable for direct use from the
 * user-side syscall handlers.
 *
 * Each `sock_create` returns a slot index; the same index is what
 * gets stored in `task_fd.obj_idx` for an FD_SOCK fd. Sockets
 * carry a tcb pointer (NULL for SOCK_NEW), so the kernel can have
 * any mix of listeners and clients up to TCP_MAX_TCBS at once.
 */

#define SOCK_MAX           24     /* bumped in session 64 to keep parity
                                    * with TCP_MAX_TCBS when the agentd
                                    * listener joined the boot service set. */
/* Session 45 bump: 2048 → 4096 to give the TLS receive path room for
 * one real-world server flight (Cloudflare's is ~2.5 KiB). The TCP
 * window we advertise = this size. Larger values (8K+) blew the
 * kernel's BSS budget — see kernel bss_end vs heap base in pmm.c. */
#define SOCK_RX_BUF        4096
#define SOCK_BACKLOG_MAX   8      /* per-listener accept-queue depth */

enum {
    SOCK_FREE = 0,
    SOCK_NEW,           /* socket() returned, not yet bound or connected */
    SOCK_LISTEN,        /* listening for connections                    */
    SOCK_CONNECTING,    /* tcp_connect issued, waiting for ESTABLISHED  */
    SOCK_CONNECTED,     /* established, read/write OK                   */
    SOCK_CLOSED,        /* peer closed and ring drained — read returns 0 */
};

struct tcb;        /* forward */

struct sock {
    int               state;
    int               refcount;     /* fork bumps, close decrements; 0 = free */
    uint16_t          port;
    struct tcb       *tcb;          /* live underlying TCP control block */

    /* For LISTEN sockets: a FIFO ring of conn-socket indices that
     * have completed the 3-way handshake and are awaiting accept.
     * `backlog` is the configured cap (set by sock_listen). When the
     * ring is full, an arriving SYN-ACK from the peer still gets
     * back-pressured by TCP — but we drop the conn from on_connect
     * since we can't queue it. tail->head order is FIFO. */
    int               pending_conns[SOCK_BACKLOG_MAX];
    int               pending_head;       /* enqueue at head */
    int               pending_tail;       /* dequeue at tail */
    int               backlog;            /* configured queue depth */

    /* For CONNECTED/CONNECTING sockets: SPSC ring buffer. Producer
     * is the TCP rx callback (IRQ context); consumer is the user
     * task in sock_read. head/tail are 32-bit aligned, atomic on
     * x86 — no lock needed. */
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    volatile int      peer_closed;
    uint8_t           rx_buf[SOCK_RX_BUF];
};

void sock_init(void);

int  sock_create (void);                                /* idx or -1 */
int  sock_bind   (int idx, uint16_t port);
int  sock_listen (int idx, int backlog);                /* backlog 1..SOCK_BACKLOG_MAX */
int  sock_accept (int idx);                             /* blocks; returns new idx */

/* Active open. dst is a 4-byte IPv4 in network order; port is host
 * order. Blocks until ESTABLISHED or connection failure. Returns 0
 * on success, -1 on error. */
int  sock_connect(int idx, const uint8_t dst[4], uint16_t port);

int  sock_read   (int idx, void *buf, int n);           /* blocks; 0 = EOF */
int  sock_write  (int idx, const void *buf, int n);
int  sock_close  (int idx);
void sock_inc_ref(int idx);                             /* fork uses this  */

/* Session 62 — non-blocking peek helpers. Lets the syscall layer
 * implement O_NONBLOCK semantics on SOCK fds without changing the
 * existing blocking accept/read signatures.
 *
 *   sock_accept_avail(idx) = 1 if a connection is already queued in
 *                            this listener's backlog ring, 0 otherwise.
 *                            Returns -1 on bad idx / wrong state.
 *
 *   sock_read_avail(idx)   = 1 if there's at least one byte in the
 *                            socket's rx ring OR the peer has closed
 *                            (so a non-blocking read would observe
 *                            either data or EOF, never block).
 *                            Returns -1 on bad idx / wrong state. */
int  sock_accept_avail(int idx);
int  sock_read_avail  (int idx);

#endif
