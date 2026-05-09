#include "ip.h"
#include "arp.h"
#include "icmp.h"
#include "tcp.h"
#include "string.h"
#include "kprintf.h"

uint16_t ip_checksum(const void *data, uint32_t len) {
    /* One's-complement sum of 16-bit half-words, with end-around carry. */
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len)        { sum += *(const uint8_t *)p; }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t g_ip_id;        /* monotonically increasing for outbound */

int ip_send(const struct ip_addr *dst, uint8_t proto,
            const void *payload, uint32_t len) {
    if (len > 1500 - sizeof(struct ip_hdr)) return -1;

    /* Choose next-hop: dst itself if local, gateway otherwise. */
    struct ip_addr  nexthop = net_is_local(dst) ? *dst : g_gateway_ip;
    struct mac_addr mac;
    if (arp_lookup(&nexthop, &mac) != 0) {
        /* Cache miss: fire an ARP request now and let the caller
         * retry (typical "ping" pattern). */
        arp_send_request(&nexthop);
        return -2;
    }

    uint8_t buf[1500];
    struct ip_hdr *iph = (struct ip_hdr *)buf;
    iph->ver_ihl    = 0x45;                              /* IPv4, 5 dword IHL */
    iph->tos        = 0;
    iph->total_len  = htons((uint16_t)(sizeof(*iph) + len));
    iph->id         = htons(++g_ip_id);
    iph->flags_frag = 0;
    iph->ttl        = 64;
    iph->proto      = proto;
    iph->csum       = 0;
    iph->src        = g_my_ip;
    iph->dst        = *dst;
    iph->csum       = ip_checksum(iph, sizeof(*iph));

    memcpy(buf + sizeof(*iph), payload, len);

    return eth_send(&mac, ETH_TYPE_IPV4, buf, sizeof(*iph) + len);
}

void ip_rx(const struct eth_hdr *eth, const void *payload, uint32_t len) {
    if (len < sizeof(struct ip_hdr)) return;
    const struct ip_hdr *iph = payload;

    if ((iph->ver_ihl >> 4) != 4) return;            /* v4 only */
    uint32_t ihl = (iph->ver_ihl & 0x0F) * 4u;
    if (ihl < sizeof(*iph))     return;
    if (ntohs(iph->total_len) > len) return;

    /* Drop anything not addressed to us (we're not a router). */
    for (int i = 0; i < 4; i++) {
        if (iph->dst.b[i] != g_my_ip.b[i]) return;
    }

    const uint8_t *body  = (const uint8_t *)payload + ihl;
    uint32_t       blen  = (uint32_t)ntohs(iph->total_len) - ihl;

    switch (iph->proto) {
        case IP_PROTO_ICMP: icmp_rx(eth, iph, body, blen); break;
        case IP_PROTO_TCP:  tcp_rx (iph, body, (int)blen); break;
        default:                                            break;
    }
}
