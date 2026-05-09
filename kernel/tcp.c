#include "tcp.h"
#include "ip.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

/* Single TCB. Real kernels keep a hash by (local_port, remote_ip,
 * remote_port) and a separate listen table — for our HTTP demo one
 * connection at a time is enough. */
static struct tcb g_tcb;
static int        g_listening;
static uint16_t   g_listen_port;

const char *tcp_state_name(int s) {
    switch (s) {
        case TCP_CLOSED:      return "CLOSED";
        case TCP_LISTEN:      return "LISTEN";
        case TCP_SYN_RCVD:    return "SYN_RCVD";
        case TCP_ESTABLISHED: return "ESTABLISHED";
        case TCP_FIN_WAIT_1:  return "FIN_WAIT_1";
        case TCP_FIN_WAIT_2:  return "FIN_WAIT_2";
        case TCP_CLOSE_WAIT:  return "CLOSE_WAIT";
        case TCP_LAST_ACK:    return "LAST_ACK";
        case TCP_TIME_WAIT:   return "TIME_WAIT";
        default:              return "?";
    }
}

void tcp_init(void) {
    memset(&g_tcb, 0, sizeof(g_tcb));
    g_listening = 0;
}

/* TCP/IP checksum. Builds the pseudo-header + segment as a flat byte
 * buffer and reuses the known-good ip_checksum() over the whole thing.
 * Avoids any subtle "compute checksum across a packed struct via
 * uint16_t-stride reads" bug — there's nowhere for byte-order or
 * struct-padding mistakes to creep in. */
static uint16_t tcp_checksum(const struct ip_addr *src,
                             const struct ip_addr *dst,
                             const void *seg, uint16_t seg_len) {
    uint8_t buf[12 + 1500];
    if (seg_len > 1500) return 0;

    /* Pseudo-header (12 bytes), explicitly written in network order. */
    memcpy(buf + 0, src, 4);
    memcpy(buf + 4, dst, 4);
    buf[8]  = 0;
    buf[9]  = IP_PROTO_TCP;
    buf[10] = (uint8_t)(seg_len >> 8);
    buf[11] = (uint8_t)(seg_len & 0xFF);

    memcpy(buf + 12, seg, seg_len);
    return ip_checksum(buf, 12u + seg_len);
}

/* Build + transmit a single TCP segment. Computes checksum, hands to
 * ip_send. Caller is responsible for snd_nxt accounting. */
static int tcp_send_segment(struct tcb *t, uint8_t flags,
                            const void *data, int len) {
    uint8_t buf[1500];
    if (sizeof(struct tcp_hdr) + len > sizeof(buf)) return -1;

    struct tcp_hdr *th = (struct tcp_hdr *)buf;
    memset(th, 0, sizeof(*th));
    th->src_port = htons(t->local_port);
    th->dst_port = htons(t->remote_port);
    th->seq      = htonl(t->snd_nxt);
    th->ack      = htonl(t->rcv_nxt);
    th->data_off = (uint8_t)(5 << 4);          /* 5 dwords = 20 bytes */
    th->flags    = flags;
    th->window   = htons(TCP_RX_BUF);
    th->csum     = 0;
    th->urgent   = 0;
    if (len) memcpy(buf + sizeof(*th), data, (size_t)len);

    th->csum = tcp_checksum(&g_my_ip, &t->remote_ip, buf,
                            (uint16_t)(sizeof(*th) + len));

    return ip_send(&t->remote_ip, IP_PROTO_TCP, buf, sizeof(*th) + len);
}

int tcp_listen(uint16_t port,
               tcp_connect_cb on_connect,
               tcp_recv_cb    on_recv,
               tcp_close_cb   on_close) {
    memset(&g_tcb, 0, sizeof(g_tcb));
    g_tcb.state      = TCP_LISTEN;
    g_tcb.local_port = port;
    g_tcb.on_connect = on_connect;
    g_tcb.on_recv    = on_recv;
    g_tcb.on_close   = on_close;
    g_listen_port    = port;
    g_listening      = 1;
    return 0;
}

int tcp_send_active(const void *data, int len) {
    if (g_tcb.state != TCP_ESTABLISHED && g_tcb.state != TCP_CLOSE_WAIT) return -1;
    return tcp_send(&g_tcb, data, len);
}

int tcp_close_active(void) {
    return tcp_close(&g_tcb);
}

int tcp_active_state(void) { return g_tcb.state; }

int tcp_send(struct tcb *t, const void *data, int len) {
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) return -1;
    int rc = tcp_send_segment(t, TCP_PSH | TCP_ACK, data, len);
    if (rc >= 0) t->snd_nxt += (uint32_t)len;
    return rc;
}

int tcp_close(struct tcb *t) {
    if (t->state == TCP_ESTABLISHED) {
        tcp_send_segment(t, TCP_FIN | TCP_ACK, NULL, 0);
        t->snd_nxt += 1;                       /* FIN consumes a sequence */
        t->state = TCP_FIN_WAIT_1;
    } else if (t->state == TCP_CLOSE_WAIT) {
        tcp_send_segment(t, TCP_FIN | TCP_ACK, NULL, 0);
        t->snd_nxt += 1;
        t->state = TCP_LAST_ACK;
    }
    return 0;
}

static void back_to_listen(struct tcb *t) {
    /* Fully close and re-arm the listener for the next client. */
    tcp_connect_cb ncb = t->on_connect;
    tcp_recv_cb    rcb = t->on_recv;
    tcp_close_cb   ccb = t->on_close;
    uint16_t       port = g_listen_port;
    if (ccb) ccb(t);
    if (g_listening) tcp_listen(port, ncb, rcb, ccb);
    else             t->state = TCP_CLOSED;
}

void tcp_rx(const struct ip_hdr *iph, const void *seg, int len) {
    if (len < (int)sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *th = seg;

    uint16_t dst_port = ntohs(th->dst_port);
    uint8_t  flags    = th->flags;
    uint32_t seq      = ntohl(th->seq);
    uint32_t ack      = ntohl(th->ack);
    int      hdr_len  = (th->data_off >> 4) * 4;
    if (hdr_len < (int)sizeof(*th) || hdr_len > len) return;

    const uint8_t *payload = (const uint8_t *)seg + hdr_len;
    int            plen    = len - hdr_len;

    /* Must be addressed to our listener (or the live connection). */
    if (g_tcb.state == TCP_LISTEN) {
        if (dst_port != g_tcb.local_port)             return;
        if (!(flags & TCP_SYN))                       return;

        /* Open: accept the SYN. */
        g_tcb.remote_ip   = iph->src;
        g_tcb.remote_port = ntohs(th->src_port);
        g_tcb.rcv_isn     = seq;
        g_tcb.rcv_nxt     = seq + 1;        /* SYN consumes 1 byte of seq */
        g_tcb.snd_isn     = (uint32_t)pit_ticks() * 2654435761u;
        g_tcb.snd_nxt     = g_tcb.snd_isn;
        g_tcb.snd_una     = g_tcb.snd_isn;

        /* SYN-ACK: seq=ISN, ack=peer.seq+1, flags=SYN|ACK. SYN itself
         * consumes one sequence number too. */
        tcp_send_segment(&g_tcb, TCP_SYN | TCP_ACK, NULL, 0);
        g_tcb.snd_nxt += 1;
        g_tcb.state = TCP_SYN_RCVD;
        return;
    }

    /* Beyond LISTEN: ignore packets that don't match the live tuple. */
    if (dst_port != g_tcb.local_port)                 return;
    if (ntohs(th->src_port) != g_tcb.remote_port)     return;
    for (int i = 0; i < 4; i++) {
        if (iph->src.b[i] != g_tcb.remote_ip.b[i])    return;
    }

    /* RST anywhere → tear down hard. */
    if (flags & TCP_RST) {
        back_to_listen(&g_tcb);
        return;
    }

    switch (g_tcb.state) {
        case TCP_SYN_RCVD: {
            /* Expect the final ACK of the handshake. */
            if (!(flags & TCP_ACK))                 return;
            if (ack != g_tcb.snd_nxt)               return;
            g_tcb.snd_una = ack;
            g_tcb.state   = TCP_ESTABLISHED;
            if (g_tcb.on_connect) g_tcb.on_connect(&g_tcb);
            /* Fall through: this segment may also have data piggybacked. */
        } /* fallthrough */
        case TCP_ESTABLISHED: {
            if (flags & TCP_ACK) g_tcb.snd_una = ack;

            if (plen > 0 && seq == g_tcb.rcv_nxt) {
                g_tcb.rcv_nxt += (uint32_t)plen;
                /* Ack the data immediately. */
                tcp_send_segment(&g_tcb, TCP_ACK, NULL, 0);
                if (g_tcb.on_recv) g_tcb.on_recv(&g_tcb,
                                                 (const char *)payload,
                                                 plen);
            }

            if (flags & TCP_FIN) {
                g_tcb.rcv_nxt += 1;
                tcp_send_segment(&g_tcb, TCP_ACK, NULL, 0);
                g_tcb.state = TCP_CLOSE_WAIT;
                /* Application should call tcp_close(). For our HTTP
                 * server it already did so before the FIN arrived; if
                 * not, we'll just sit in CLOSE_WAIT. */
            }
            break;
        }
        case TCP_FIN_WAIT_1: {
            /* We sent FIN; expecting ACK and/or peer's FIN. */
            if ((flags & TCP_ACK) && ack == g_tcb.snd_nxt) {
                g_tcb.snd_una = ack;
                g_tcb.state   = TCP_FIN_WAIT_2;
            }
            if (flags & TCP_FIN) {
                g_tcb.rcv_nxt += 1;
                tcp_send_segment(&g_tcb, TCP_ACK, NULL, 0);
                /* If we hadn't gotten the ACK yet, this is "simultaneous
                 * close" — skip TIME_WAIT for our minimum impl. */
                back_to_listen(&g_tcb);
            }
            break;
        }
        case TCP_FIN_WAIT_2: {
            if (flags & TCP_FIN) {
                g_tcb.rcv_nxt += 1;
                tcp_send_segment(&g_tcb, TCP_ACK, NULL, 0);
                back_to_listen(&g_tcb);
            }
            break;
        }
        case TCP_CLOSE_WAIT:
            /* Waiting for our app to call tcp_close. Nothing inbound
             * to do besides ACK keepalives. */
            break;
        case TCP_LAST_ACK: {
            if ((flags & TCP_ACK) && ack == g_tcb.snd_nxt) {
                back_to_listen(&g_tcb);
            }
            break;
        }
        default:
            break;
    }
}
