#include "tcp.h"
#include "ip.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

/*
 * Multi-TCB TCP. Each TCB is a self-contained connection state
 * machine; tcp_rx looks up the right one by 4-tuple (or by
 * dst_port + LISTEN for SYNs). The g_tcbs array is small (4
 * slots) — enough for httpd plus 1-2 outbound clients running
 * at the same time.
 */

static struct tcb g_tcbs[TCP_MAX_TCBS];

/* Round-robin counter for ephemeral local-port allocation. Real
 * stacks pick from 32768..60999 (Linux default range); we just
 * cycle through 49152..65535 and skip ports already in use by
 * a live TCB. */
static uint16_t g_next_eph = 49152;

const char *tcp_state_name(int s) {
    switch (s) {
        case TCP_CLOSED:      return "CLOSED";
        case TCP_LISTEN:      return "LISTEN";
        case TCP_SYN_SENT:    return "SYN_SENT";
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
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        memset(&g_tcbs[i], 0, sizeof(g_tcbs[i]));
        g_tcbs[i].in_use = 0;
    }
    g_next_eph = 49152;
}

static struct tcb *tcb_alloc(void) {
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        if (!g_tcbs[i].in_use) {
            memset(&g_tcbs[i], 0, sizeof(g_tcbs[i]));
            g_tcbs[i].in_use = 1;
            g_tcbs[i].state  = TCP_CLOSED;
            return &g_tcbs[i];
        }
    }
    return 0;
}

static void tcb_release(struct tcb *t) {
    t->in_use      = 0;
    t->state       = TCP_CLOSED;
    t->is_listener = 0;
    t->on_connect  = 0;
    t->on_recv     = 0;
    t->on_close    = 0;
    t->user_data   = 0;
}

/* True if `port` is currently bound to any non-CLOSED TCB. */
static int port_in_use(uint16_t port) {
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        if (!g_tcbs[i].in_use) continue;
        if (g_tcbs[i].state == TCP_CLOSED) continue;
        if (g_tcbs[i].local_port == port) return 1;
    }
    return 0;
}

static uint16_t alloc_ephemeral_port(void) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = g_next_eph++;
        if (g_next_eph == 0) g_next_eph = 49152;
        if (p < 49152) p = 49152;
        if (!port_in_use(p)) return p;
    }
    return 0;
}

/* TCP/IP checksum. Builds the pseudo-header + segment as a flat byte
 * buffer and reuses ip_checksum() over the whole thing. Avoids any
 * subtle "compute checksum across a packed struct via uint16_t-stride
 * reads" bug — there's nowhere for byte-order or struct-padding
 * mistakes to creep in. */
static uint16_t tcp_checksum(const struct ip_addr *src,
                             const struct ip_addr *dst,
                             const void *seg, uint16_t seg_len) {
    uint8_t buf[12 + 1500];
    if (seg_len > 1500) return 0;

    memcpy(buf + 0, src, 4);
    memcpy(buf + 4, dst, 4);
    buf[8]  = 0;
    buf[9]  = IP_PROTO_TCP;
    buf[10] = (uint8_t)(seg_len >> 8);
    buf[11] = (uint8_t)(seg_len & 0xFF);

    memcpy(buf + 12, seg, seg_len);
    return ip_checksum(buf, 12u + seg_len);
}

/* Build + transmit a single TCP segment with the given seq number.
 * Loopback (session 29) means ip_send may synchronously recurse
 * through tcp_rx before this call returns — so the CALLER must
 * have already advanced t->snd_nxt past the bytes this segment
 * consumes BEFORE invoking us. Otherwise the peer's ACK arrives
 * (via recursion) checking against an unincremented snd_nxt and
 * gets dropped. The seq parameter is used directly for the
 * segment's header so callers can still send the right value
 * even after pre-incrementing snd_nxt. */
static int tcp_send_seg_seq(struct tcb *t, uint8_t flags,
                            const void *data, int len, uint32_t seq) {
    uint8_t buf[1500];
    if (sizeof(struct tcp_hdr) + len > sizeof(buf)) return -1;

    struct tcp_hdr *th = (struct tcp_hdr *)buf;
    memset(th, 0, sizeof(*th));
    th->src_port = htons(t->local_port);
    th->dst_port = htons(t->remote_port);
    th->seq      = htonl(seq);
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

/* Convenience wrapper for ACK-only segments (no sequence-space
 * consumption): pre-increment is unnecessary, just send with the
 * current snd_nxt. */
static int tcp_send_ack(struct tcb *t, uint8_t flags) {
    return tcp_send_seg_seq(t, flags, NULL, 0, t->snd_nxt);
}

struct tcb *tcp_listen(uint16_t port,
                       tcp_connect_cb on_connect,
                       tcp_recv_cb    on_recv,
                       tcp_close_cb   on_close,
                       void          *user_data) {
    if (port_in_use(port)) return 0;
    struct tcb *t = tcb_alloc();
    if (!t) return 0;

    t->state       = TCP_LISTEN;
    t->is_listener = 1;
    t->local_port  = port;
    t->on_connect  = on_connect;
    t->on_recv     = on_recv;
    t->on_close    = on_close;
    t->user_data   = user_data;
    return t;
}

struct tcb *tcp_connect(const struct ip_addr *remote_ip,
                        uint16_t              remote_port,
                        tcp_connect_cb        on_connect,
                        tcp_recv_cb           on_recv,
                        tcp_close_cb          on_close,
                        void                 *user_data) {
    if (!remote_ip) return 0;

    uint16_t local_port = alloc_ephemeral_port();
    if (!local_port) return 0;

    struct tcb *t = tcb_alloc();
    if (!t) return 0;

    t->state       = TCP_SYN_SENT;
    t->is_listener = 0;
    t->local_port  = local_port;
    t->remote_ip   = *remote_ip;
    t->remote_port = remote_port;
    t->snd_isn     = (uint32_t)pit_ticks() * 2654435761u;
    t->snd_nxt     = t->snd_isn;
    t->snd_una     = t->snd_isn;
    t->rcv_nxt     = 0;
    t->on_connect  = on_connect;
    t->on_recv     = on_recv;
    t->on_close    = on_close;
    t->user_data   = user_data;

    /* Send SYN. SYN consumes one sequence number, so we advance
     * snd_nxt FIRST and pass the original ISN to the segment
     * builder. Without this pre-advance, a loopback recursion
     * sees the peer's ACK arrive while snd_nxt is still ISN and
     * drops it as "ack != snd_nxt". */
    uint32_t syn_seq = t->snd_nxt;
    t->snd_nxt += 1;
    if (tcp_send_seg_seq(t, TCP_SYN, NULL, 0, syn_seq) < 0) {
        tcb_release(t);
        return 0;
    }
    return t;
}

int tcp_send(struct tcb *t, const void *data, int len) {
    if (!t || !t->in_use) return -1;
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) return -1;
    /* Pre-advance snd_nxt so a loopback ACK can match. */
    uint32_t seq = t->snd_nxt;
    t->snd_nxt += (uint32_t)len;
    int rc = tcp_send_seg_seq(t, TCP_PSH | TCP_ACK, data, len, seq);
    if (rc < 0) {
        t->snd_nxt = seq;             /* roll back on failure */
        return -1;
    }
    /* Return the byte count accepted, not ip_send's "ok" 0. Without
     * this fix, a write_all loop (e.g., libcrypto/tls.c's record
     * sender) sees sys_write return 0 and treats it as "wrote
     * nothing — retry forever". httpd/wget were lucky: they don't
     * check sys_write's return value. TLS does, and must. */
    return len;
}

int tcp_close(struct tcb *t) {
    if (!t || !t->in_use) return -1;
    if (t->state == TCP_LISTEN) {
        /* Listener: just retire the TCB. */
        tcb_release(t);
        return 0;
    }
    if (t->state == TCP_ESTABLISHED) {
        uint32_t fin_seq = t->snd_nxt;
        t->snd_nxt += 1;                       /* FIN consumes a sequence */
        t->state = TCP_FIN_WAIT_1;
        tcp_send_seg_seq(t, TCP_FIN | TCP_ACK, NULL, 0, fin_seq);
    } else if (t->state == TCP_CLOSE_WAIT) {
        uint32_t fin_seq = t->snd_nxt;
        t->snd_nxt += 1;
        t->state = TCP_LAST_ACK;
        tcp_send_seg_seq(t, TCP_FIN | TCP_ACK, NULL, 0, fin_seq);
    } else if (t->state == TCP_SYN_SENT) {
        /* Half-open close: just drop. */
        tcb_release(t);
    }
    return 0;
}

int tcp_state_of(const struct tcb *t) {
    if (!t || !t->in_use) return TCP_CLOSED;
    return t->state;
}

/* Demultiplex an incoming segment to a TCB. Tries an exact
 * 4-tuple match first; if none, falls back to a LISTEN match
 * on dst_port (only for SYN segments). Returns NULL if the
 * segment doesn't match any TCB. */
static struct tcb *find_tcb_for_seg(uint16_t       dst_port,
                                    uint16_t       src_port,
                                    const struct ip_addr *src_ip,
                                    uint8_t        flags) {
    /* Exact match first (excludes LISTEN). */
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        struct tcb *t = &g_tcbs[i];
        if (!t->in_use) continue;
        if (t->state == TCP_LISTEN)              continue;
        if (t->state == TCP_CLOSED)              continue;
        if (t->local_port  != dst_port)          continue;
        if (t->remote_port != src_port)          continue;
        int ok = 1;
        for (int b = 0; b < 4; b++) {
            if (t->remote_ip.b[b] != src_ip->b[b]) { ok = 0; break; }
        }
        if (ok) return t;
    }
    /* LISTEN fallback for SYNs. */
    if (flags & TCP_SYN) {
        for (int i = 0; i < TCP_MAX_TCBS; i++) {
            struct tcb *t = &g_tcbs[i];
            if (!t->in_use)                    continue;
            if (t->state != TCP_LISTEN)        continue;
            if (t->local_port == dst_port)     return t;
        }
    }
    return 0;
}

/* On listener-SYN arrival: spawn a new conn TCB inheriting the
 * listener's callbacks, send SYN-ACK, return the new TCB (or NULL
 * if out of slots). */
static struct tcb *spawn_conn_from_listener(struct tcb *l,
                                            const struct ip_addr *src_ip,
                                            uint16_t              src_port,
                                            uint32_t              seq) {
    struct tcb *c = tcb_alloc();
    if (!c) return 0;

    c->state       = TCP_SYN_RCVD;
    c->is_listener = 0;
    c->local_port  = l->local_port;
    c->remote_ip   = *src_ip;
    c->remote_port = src_port;
    c->rcv_isn     = seq;
    c->rcv_nxt     = seq + 1;                  /* SYN consumes 1 byte */
    c->snd_isn     = (uint32_t)pit_ticks() * 2654435761u;
    c->snd_nxt     = c->snd_isn;
    c->snd_una     = c->snd_isn;
    c->on_connect  = l->on_connect;
    c->on_recv     = l->on_recv;
    c->on_close    = l->on_close;
    c->user_data   = l->user_data;

    /* Pre-advance like tcp_connect: SYN-ACK's seq is the ISN, but
     * snd_nxt must already be ISN+1 by the time the peer's final
     * ACK arrives via loopback recursion. */
    uint32_t syn_seq = c->snd_nxt;
    c->snd_nxt += 1;
    tcp_send_seg_seq(c, TCP_SYN | TCP_ACK, NULL, 0, syn_seq);
    return c;
}

void tcp_rx(const struct ip_hdr *iph, const void *seg, int len) {
    if (len < (int)sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *th = seg;

    uint16_t dst_port = ntohs(th->dst_port);
    uint16_t src_port = ntohs(th->src_port);
    uint8_t  flags    = th->flags;
    uint32_t seq      = ntohl(th->seq);
    uint32_t ack      = ntohl(th->ack);
    int      hdr_len  = (th->data_off >> 4) * 4;
    if (hdr_len < (int)sizeof(*th) || hdr_len > len) return;

    const uint8_t *payload = (const uint8_t *)seg + hdr_len;
    int            plen    = len - hdr_len;

    struct tcb *t = find_tcb_for_seg(dst_port, src_port, &iph->src, flags);
    if (!t) return;

    /* Listener: a fresh SYN arrived. Spawn a new conn TCB. */
    if (t->state == TCP_LISTEN) {
        if (!(flags & TCP_SYN)) return;
        spawn_conn_from_listener(t, &iph->src, src_port, seq);
        return;
    }

    /* RST anywhere → tear down hard. */
    if (flags & TCP_RST) {
        if (t->on_close) t->on_close(t);
        tcb_release(t);
        return;
    }

    switch (t->state) {
        case TCP_SYN_SENT: {
            /* Active open: expecting SYN-ACK. */
            if (!(flags & TCP_SYN) || !(flags & TCP_ACK))      return;
            if (ack != t->snd_nxt)                             return;
            t->snd_una = ack;
            t->rcv_isn = seq;
            t->rcv_nxt = seq + 1;
            /* Send the final ACK to complete the handshake. */
            tcp_send_ack(t, TCP_ACK);
            t->state = TCP_ESTABLISHED;
            if (t->on_connect) t->on_connect(t);
            /* Data may have been piggybacked. */
            if (plen > 0) {
                t->rcv_nxt += (uint32_t)plen;
                tcp_send_ack(t, TCP_ACK);
                if (t->on_recv) t->on_recv(t, (const char *)payload, plen);
            }
            break;
        }
        case TCP_SYN_RCVD: {
            /* Expect the final ACK of the handshake. */
            if (!(flags & TCP_ACK))                 return;
            if (ack != t->snd_nxt)                  return;
            t->snd_una = ack;
            t->state   = TCP_ESTABLISHED;
            if (t->on_connect) t->on_connect(t);
            /* Fall through: this segment may also have data piggybacked. */
        } /* fallthrough */
        case TCP_ESTABLISHED: {
            if (flags & TCP_ACK) t->snd_una = ack;

            if (plen > 0 && seq == t->rcv_nxt) {
                t->rcv_nxt += (uint32_t)plen;
                tcp_send_ack(t, TCP_ACK);
                if (t->on_recv) t->on_recv(t, (const char *)payload, plen);
            }

            if (flags & TCP_FIN) {
                t->rcv_nxt += 1;
                tcp_send_ack(t, TCP_ACK);
                t->state = TCP_CLOSE_WAIT;
                if (t->on_close) t->on_close(t);
            }
            break;
        }
        case TCP_FIN_WAIT_1: {
            /* We sent FIN; expecting ACK and/or peer's FIN. */
            if ((flags & TCP_ACK) && ack == t->snd_nxt) {
                t->snd_una = ack;
                t->state   = TCP_FIN_WAIT_2;
            }
            if (flags & TCP_FIN) {
                t->rcv_nxt += 1;
                tcp_send_ack(t, TCP_ACK);
                if (t->on_close) t->on_close(t);
                tcb_release(t);
            }
            break;
        }
        case TCP_FIN_WAIT_2: {
            if (flags & TCP_FIN) {
                t->rcv_nxt += 1;
                tcp_send_ack(t, TCP_ACK);
                if (t->on_close) t->on_close(t);
                tcb_release(t);
            }
            break;
        }
        case TCP_CLOSE_WAIT:
            /* Waiting for our app to call tcp_close. ACK keepalives. */
            break;
        case TCP_LAST_ACK: {
            if ((flags & TCP_ACK) && ack == t->snd_nxt) {
                if (t->on_close) t->on_close(t);
                tcb_release(t);
            }
            break;
        }
        default:
            break;
    }
}
