#ifndef ADVENTOS_TCP_H
#define ADVENTOS_TCP_H

#include "../include/types.h"
#include "net.h"
#include "ip.h"

/*
 * Minimum-viable TCP. Supports both passive (LISTEN) and active
 * (CONNECT) opens against a small pool of TCBs. Each TCB carries
 * its own connection state machine, user_data pointer, and
 * callbacks. tcp_rx demultiplexes by 4-tuple (or by dst_port +
 * LISTEN state for SYNs).
 *
 * No retransmission, no congestion control, fixed window. Enough
 * to drive multiple short-lived HTTP / IRC / nc-style connections
 * over loopback or SLIRP at a time.
 */

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

/* Bumped 2048 → 4096 in session 45 so the advertised TCP window can
 * hold a real-world TLS 1.3 server flight (cert chain ~2.5 KiB). */
#define TCP_RX_BUF 4096
#define TCP_TX_BUF 2048

/* Pool size — bumped to 16 in session 30 for fork-per-conn httpd.
 * Each accepted connection holds 2 TCBs (server-side conn + client-
 * side conn for loopback tests). 1 LISTEN + 3 concurrent loopback
 * connections = 7 TCBs minimum; bumped to 16 with headroom.
 *
 * Session 64 added the agentd listener — fourth permanent LISTEN TCB
 * alongside httpd/httpsd/sshd, plus the t47 selftest opens its own
 * connection. With 4 listeners + the t21 multi-conn TCP test's three
 * concurrent server-side TCBs + headroom we ran out at 16; bumped
 * to 24. */
#define TCP_MAX_TCBS 24

enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,        /* outbound: SYN sent, waiting for SYN-ACK */
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;     /* high 4 bits = header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t csum;
    uint16_t urgent;
} __attribute__((packed));

struct tcb;
typedef void (*tcp_connect_cb)(struct tcb *t);                          /* fires when state -> ESTABLISHED */
typedef void (*tcp_recv_cb)   (struct tcb *t, const char *data, int len);
typedef void (*tcp_close_cb)  (struct tcb *t);

struct tcb {
    int            in_use;
    int            state;
    int            is_listener;       /* 1 if originally a passive open */
    uint16_t       local_port;
    struct ip_addr remote_ip;
    uint16_t       remote_port;

    /* Sequence-space bookkeeping. snd_isn is what we picked, snd_nxt
     * is the next byte we'll send, snd_una is the oldest byte we've
     * sent that hasn't been ACKed. rcv_nxt is the next byte we
     * expect to receive. */
    uint32_t       snd_isn, snd_nxt, snd_una;
    uint32_t       rcv_isn, rcv_nxt;

    /* Application callbacks. */
    tcp_connect_cb on_connect;
    tcp_recv_cb    on_recv;
    tcp_close_cb   on_close;
    void          *user_data;          /* opaque cookie for the app */

    /* Inbound bytes go straight to on_recv; we don't buffer them.
     * tx_buf is reserved for queued sends. */
    char           tx_buf[TCP_TX_BUF];
    int            tx_len;
};

void tcp_init(void);

/* Bind a passive listener at `port`. on_connect fires (with a freshly
 * allocated conn TCB) the moment the 3-way handshake completes; the
 * listener TCB stays in LISTEN so subsequent SYNs spawn new conns.
 *
 * Returns the listener TCB or NULL on out-of-tcb. */
struct tcb *tcp_listen(uint16_t port,
                       tcp_connect_cb on_connect,
                       tcp_recv_cb    on_recv,
                       tcp_close_cb   on_close,
                       void          *user_data);

/* Active open: pick an ephemeral local port, allocate a TCB, send
 * SYN to (remote_ip, remote_port). Transitions through SYN_SENT;
 * on_connect fires when the SYN-ACK completes the handshake.
 *
 * Returns the conn TCB or NULL on out-of-tcb / send failure. The
 * caller can poll tcp_state_of() to wait for ESTABLISHED. */
struct tcb *tcp_connect(const struct ip_addr *remote_ip,
                        uint16_t              remote_port,
                        tcp_connect_cb        on_connect,
                        tcp_recv_cb           on_recv,
                        tcp_close_cb          on_close,
                        void                 *user_data);

int  tcp_send (struct tcb *t, const void *data, int len);
int  tcp_close(struct tcb *t);
int  tcp_state_of(const struct tcb *t);

/* Called by ip_rx when protocol == 6. */
void tcp_rx(const struct ip_hdr *iph, const void *seg, int len);

const char *tcp_state_name(int s);

#endif
