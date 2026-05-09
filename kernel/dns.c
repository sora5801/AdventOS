#include "dns.h"
#include "udp.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"

#define DNS_PORT     53

#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1

#define DNS_RESPONSE_TIMEOUT_MS 2000

struct dns_hdr {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

/* Volatile state for the synchronous wait. The UDP listener fills
 * g_resp_buf[] in IRQ context, then sets g_have_resp; the polling
 * caller spins on the flag. */
static volatile int g_have_resp;
static uint8_t      g_resp_buf[1500];
static int          g_resp_len;
static volatile uint16_t g_expecting_id;

void dns_init(void) {
    g_have_resp = 0;
    /* Listener runs on a high port — pick something in the ephemeral
     * range. (We don't have a port allocator yet.) */
    udp_listen(53000, NULL);     /* placeholder; real registration in resolve() */
}

static void on_dns_reply(const struct ip_addr *src, uint16_t src_port,
                         const void *data, int len) {
    (void)src; (void)src_port;
    if (len < (int)sizeof(struct dns_hdr)) return;
    const struct dns_hdr *h = data;
    if (ntohs(h->id) != g_expecting_id)    return;
    if (len > (int)sizeof(g_resp_buf))     return;

    memcpy(g_resp_buf, data, (size_t)len);
    g_resp_len  = len;
    g_have_resp = 1;
}

/* Encode a hostname into the DNS wire format: each dot-separated
 * label is preceded by its 1-byte length; trailing zero terminator.
 * "google.com" -> [6]google[3]com[0]. Returns the encoded length, or
 * -1 if `dst` is too small. */
static int encode_name(const char *name, uint8_t *dst, int cap) {
    int o = 0;
    const char *p = name;
    while (*p) {
        const char *start = p;
        while (*p && *p != '.') p++;
        int len = (int)(p - start);
        if (len == 0 || len > 63)         return -1;
        if (o + 1 + len + 1 > cap)        return -1;
        dst[o++] = (uint8_t)len;
        for (int i = 0; i < len; i++) dst[o++] = (uint8_t)start[i];
        if (*p == '.') p++;
    }
    if (o + 1 > cap) return -1;
    dst[o++] = 0;
    return o;
}

/* Skip past a name in a DNS message (handles 0xC0xx pointer
 * compression). Returns the number of bytes consumed at `at`, or -1
 * on parse error. */
static int skip_name(const uint8_t *msg, int msg_len, int at) {
    int p = at;
    while (p < msg_len) {
        uint8_t b = msg[p];
        if (b == 0)             { return p - at + 1; }
        if ((b & 0xC0) == 0xC0) { return p - at + 2; }
        if (p + 1 + b >= msg_len) return -1;
        p += 1 + b;
    }
    return -1;
}

int dns_resolve(const char *name, struct ip_addr *out) {
    if (!name || !out) return -1;
    if (g_dns_server.b[0] == 0 && g_dns_server.b[1] == 0 &&
        g_dns_server.b[2] == 0 && g_dns_server.b[3] == 0) return -1;

    /* Build the query packet. */
    uint8_t pkt[512];
    struct dns_hdr *h = (struct dns_hdr *)pkt;
    g_expecting_id = (uint16_t)(pit_ticks() & 0xFFFFu);
    if (g_expecting_id == 0) g_expecting_id = 1;
    h->id      = htons(g_expecting_id);
    h->flags   = htons(0x0100);          /* standard query, recursion desired */
    h->qdcount = htons(1);
    h->ancount = 0;
    h->nscount = 0;
    h->arcount = 0;

    int off = sizeof(*h);
    int n = encode_name(name, pkt + off, (int)sizeof(pkt) - off - 4);
    if (n < 0) return -1;
    off += n;

    /* QTYPE = A, QCLASS = IN. */
    pkt[off++] = 0; pkt[off++] = DNS_TYPE_A;
    pkt[off++] = 0; pkt[off++] = DNS_CLASS_IN;

    /* Register listener + send. Source port is arbitrary above 1023. */
    g_have_resp = 0;
    g_resp_len  = 0;
    udp_listen(53000, on_dns_reply);

    if (udp_send(&g_dns_server, 53000, DNS_PORT, pkt, off) < 0) return -1;

    /* Spin until the listener marks us done. */
    int waited = 0;
    while (!g_have_resp && waited < DNS_RESPONSE_TIMEOUT_MS) {
        pit_sleep(20);
        waited += 20;
    }
    if (!g_have_resp) return -1;

    /* Parse the response. Skip the header + question, then walk the
     * answer section looking for the first A record. */
    if (g_resp_len < (int)sizeof(struct dns_hdr)) return -1;
    const uint8_t *m  = g_resp_buf;
    int            mn = g_resp_len;
    int            p  = sizeof(struct dns_hdr);

    int qdcount = ntohs(h->qdcount);
    int ancount = ntohs(((const struct dns_hdr *)m)->ancount);
    if (qdcount != 1 || ancount < 1) return -1;

    /* Skip the question. */
    int sn = skip_name(m, mn, p);
    if (sn < 0) return -1;
    p += sn + 4;                           /* +qtype +qclass */
    if (p > mn) return -1;

    /* Walk answers. */
    for (int a = 0; a < ancount; a++) {
        sn = skip_name(m, mn, p);
        if (sn < 0)            return -1;
        p += sn;
        if (p + 10 > mn)       return -1;
        uint16_t type   = (uint16_t)((m[p] << 8) | m[p+1]);
        /* class @ p+2, ttl @ p+4..p+7, rdlength @ p+8..p+9 */
        uint16_t rdlen  = (uint16_t)((m[p+8] << 8) | m[p+9]);
        p += 10;
        if (p + rdlen > mn)    return -1;

        if (type == DNS_TYPE_A && rdlen == 4) {
            out->b[0] = m[p+0]; out->b[1] = m[p+1];
            out->b[2] = m[p+2]; out->b[3] = m[p+3];
            return 0;
        }
        p += rdlen;
    }
    return -1;
}
