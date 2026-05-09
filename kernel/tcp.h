#ifndef ADVENTOS_TCP_H
#define ADVENTOS_TCP_H

#include "../include/types.h"
#include "net.h"
#include "ip.h"

/*
 * Minimum-viable TCP for server-side use. Single concurrent connection
 * per listener. No retransmission, no congestion control, fixed window.
 * Enough to serve one HTTP/1.0 request and close cleanly over loopback
 * / SLIRP.
 */

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define TCP_RX_BUF 2048
#define TCP_TX_BUF 2048

enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
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
typedef void (*tcp_connect_cb)(struct tcb *t);                          /* fires when SYN_RCVD -> ESTABLISHED */
typedef void (*tcp_recv_cb)   (struct tcb *t, const char *data, int len);
typedef void (*tcp_close_cb)  (struct tcb *t);

struct tcb {
    int            state;
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

    /* Inbound bytes are passed straight to on_recv; we don't actually
     * buffer them. tx_buf is used for queued sends. */
    char           tx_buf[TCP_TX_BUF];
    int            tx_len;
};

void tcp_init(void);

/* Bind a single listener at `port`. `on_connect` fires the moment the
 * 3-way handshake completes; `on_recv` fires for each data segment;
 * `on_close` fires on connection teardown (either side). */
int  tcp_listen(uint16_t port,
                tcp_connect_cb on_connect,
                tcp_recv_cb    on_recv,
                tcp_close_cb   on_close);

/* Send up to `len` bytes on an established connection. */
int  tcp_send(struct tcb *t, const void *data, int len);

/* Initiate close (send FIN + ACK). */
int  tcp_close(struct tcb *t);

/* Helpers for layers that don't have their own TCB pointer. We only
 * have one TCB total in this minimal stack. */
int  tcp_send_active (const void *data, int len);
int  tcp_close_active(void);
int  tcp_active_state(void);

/* Called by ip_rx when protocol == 6. */
void tcp_rx(const struct ip_hdr *iph, const void *seg, int len);

const char *tcp_state_name(int s);

#endif
