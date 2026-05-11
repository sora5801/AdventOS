/*
 * Session 60 — SNTP client + test responder.
 * See ntp.h for the protocol shape and scope boundaries.
 */
#include "ntp.h"
#include "udp.h"
#include "ip.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"

#define NTP_PORT     123
/* Seconds between the NTP epoch (1900-01-01) and the Unix epoch
 * (1970-01-01).  70 years of 365.25 days. */
#define NTP_UNIX_OFFSET  2208988800u

/* SNTP wire packet — 48 bytes. */
struct ntp_pkt {
    uint8_t  flags;             /* LI<<6 | VN<<3 | Mode */
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;        /* signed fixed-point 16.16 */
    uint32_t root_dispersion;
    uint32_t reference_id;
    uint32_t ref_ts_sec, ref_ts_frac;
    uint32_t orig_ts_sec, orig_ts_frac;
    uint32_t rx_ts_sec,   rx_ts_frac;
    uint32_t tx_ts_sec,   tx_ts_frac;
} __attribute__((packed));

/* Volatile state for the synchronous wait — same shape as dns.c's
 * response handling. The udp_listen callback runs at IRQ time, drops
 * the reply into g_resp + g_have_resp, and the caller polls. */
static volatile int       g_have_resp;
static struct ntp_pkt     g_resp;
static volatile uint32_t  g_expecting_orig_sec;

static void on_ntp_reply(const struct ip_addr *src, uint16_t src_port,
                         const void *data, int len) {
    (void)src; (void)src_port;
    if (len < (int)sizeof(struct ntp_pkt)) return;
    const struct ntp_pkt *p = (const struct ntp_pkt *)data;
    /* Match the originate timestamp we placed in the request — protects
     * against an old/stray reply hitting us. */
    if (p->orig_ts_sec != g_expecting_orig_sec) return;
    g_resp = *p;
    g_have_resp = 1;
}

/* Ephemeral source port. NTP servers respond from 123 → us:src_port. */
#define NTP_SRC_PORT  12300

int ntp_sync(const struct ip_addr *server, uint32_t *out_epoch) {
    if (!server || !out_epoch) return -1;

    /* Build the request: zero everything except the flags byte and
     * the Transmit Timestamp (which the server echoes as Originate).
     * Flags: LI=0, VN=4 (current SNTPv4), Mode=3 (client). */
    struct ntp_pkt req;
    for (size_t i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.flags = (0 << 6) | (4 << 3) | 3;
    /* Use the current PIT-derived ticks as the originate timestamp
     * marker.  Big-endian on the wire — htonl. */
    uint32_t marker = pit_ticks();
    req.tx_ts_sec = htonl(marker);
    g_expecting_orig_sec = req.tx_ts_sec;     /* keep network byte order */

    g_have_resp = 0;
    udp_listen(NTP_SRC_PORT, on_ntp_reply);

    if (udp_send(server, NTP_SRC_PORT, NTP_PORT, &req, sizeof(req)) < 0)
        return -1;

    /* Wait up to 2 s for the reply. */
    int waited = 0;
    while (!g_have_resp && waited < 2000) {
        pit_sleep(20);
        waited += 20;
    }
    if (!g_have_resp) return -1;

    /* Extract the Transmit Timestamp the server filled in. */
    uint32_t ntp_sec = ntohl(g_resp.tx_ts_sec);
    if (ntp_sec < NTP_UNIX_OFFSET) return -1;   /* sanity */
    *out_epoch = ntp_sec - NTP_UNIX_OFFSET;
    return 0;
}

/* ---- Test responder ------------------------------------------- */

static volatile int g_test_responder_on;
static volatile uint32_t g_test_responder_epoch;

static void on_test_query(const struct ip_addr *src, uint16_t src_port,
                          const void *data, int len) {
    if (!g_test_responder_on) return;
    if (len < (int)sizeof(struct ntp_pkt)) return;

    const struct ntp_pkt *q = (const struct ntp_pkt *)data;
    struct ntp_pkt reply;
    for (size_t i = 0; i < sizeof(reply); i++) ((uint8_t *)&reply)[i] = 0;
    reply.flags = (0 << 6) | (4 << 3) | 4;   /* mode=4 server */
    reply.stratum = 1;                       /* primary reference */
    reply.poll    = 4;
    reply.precision = -20;                   /* arbitrary */
    /* reference_id "ADVO" in big-endian ASCII — visible in tcpdump */
    reply.reference_id = htonl(0x4144564Fu);
    /* Originate echoes the client's transmit timestamp; client matches
     * this back. */
    reply.orig_ts_sec  = q->tx_ts_sec;
    reply.orig_ts_frac = q->tx_ts_frac;
    /* Transmit = our "current" epoch + NTP offset. Hardcoded second-
     * accurate; we don't bother with fractional. */
    uint32_t ntp_sec = g_test_responder_epoch + NTP_UNIX_OFFSET;
    reply.rx_ts_sec  = htonl(ntp_sec);
    reply.tx_ts_sec  = htonl(ntp_sec);
    reply.ref_ts_sec = htonl(ntp_sec);
    /* Send back to (src, src_port). */
    udp_send(src, NTP_PORT, src_port, &reply, sizeof(reply));
}

void ntp_test_responder(int enable, uint32_t epoch_to_return) {
    if (enable) {
        g_test_responder_epoch = epoch_to_return;
        g_test_responder_on    = 1;
        udp_listen(NTP_PORT, on_test_query);
    } else {
        g_test_responder_on    = 0;
        udp_listen(NTP_PORT, NULL);
    }
}
