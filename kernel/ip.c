#include "ip.h"
#include "arp.h"
#include "icmp.h"
#include "tcp.h"
#include "udp.h"
#include "string.h"
#include "kprintf.h"

/* Broadcast destination — used by DHCP-style "I don't have an IP yet"
 * traffic. We bypass ARP and put the Ethernet broadcast MAC directly. */
static int is_ip_broadcast(const struct ip_addr *ip) {
    return ip->b[0] == 255 && ip->b[1] == 255 &&
           ip->b[2] == 255 && ip->b[3] == 255;
}

static int we_have_ip(void) {
    return g_my_ip.b[0] || g_my_ip.b[1] || g_my_ip.b[2] || g_my_ip.b[3];
}

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

/* Loopback short-circuit (session 29). If `dst` is 127.0.0.0/8 or
 * our own IP, deliver the packet locally — synthesize an ip_hdr
 * and dispatch to the right protocol's rx handler — instead of
 * pushing it through the NIC. SLIRP does not loopback packets
 * sent to the guest's own assigned IP, so without this nc /
 * wget / irc-client-against-localhost-server scenarios fail.
 *
 * The dispatch is bounded-depth (TCP handshake = 3-4 levels of
 * recursion); we cli around it so a real-NIC packet IRQ doesn't
 * race the loopback handler on g_tcbs. */
static int try_loopback(const struct ip_addr *dst, uint8_t proto,
                        const void *payload, uint32_t len) {
    int is_lo = (dst->b[0] == 127);
    int is_us = we_have_ip() &&
                dst->b[0] == g_my_ip.b[0] &&
                dst->b[1] == g_my_ip.b[1] &&
                dst->b[2] == g_my_ip.b[2] &&
                dst->b[3] == g_my_ip.b[3];
    if (!is_lo && !is_us) return 0;

    struct ip_hdr lhdr;
    lhdr.ver_ihl    = 0x45;
    lhdr.tos        = 0;
    lhdr.total_len  = htons((uint16_t)(sizeof(lhdr) + len));
    lhdr.id         = 0;
    lhdr.flags_frag = 0;
    lhdr.ttl        = 64;
    lhdr.proto      = proto;
    lhdr.csum       = 0;
    /* Fill src as `dst` so the receiver sees self-addressed
     * packets symmetrically. */
    lhdr.src        = *dst;
    lhdr.dst        = *dst;

    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    switch (proto) {
        case IP_PROTO_TCP: tcp_rx(&lhdr, payload, (int)len); break;
        case IP_PROTO_UDP: udp_rx(&lhdr, payload, (int)len); break;
        default: break;
    }
    __asm__ volatile ("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
    return 1;
}

int ip_send(const struct ip_addr *dst, uint8_t proto,
            const void *payload, uint32_t len) {
    if (len > 1500 - sizeof(struct ip_hdr)) return -1;

    if (try_loopback(dst, proto, payload, len)) return 0;

    struct mac_addr mac;
    if (is_ip_broadcast(dst)) {
        /* Skip ARP — broadcast goes to ff:ff:ff:ff:ff:ff. This is
         * how DHCP-DISCOVER reaches the server before we have an
         * IP / gateway / ARP cache. */
        for (int i = 0; i < 6; i++) mac.b[i] = 0xff;
    } else {
        /* Choose next-hop: dst itself if local, gateway otherwise. */
        struct ip_addr nexthop = net_is_local(dst) ? *dst : g_gateway_ip;
        if (arp_lookup(&nexthop, &mac) != 0) {
            /* Cache miss: fire an ARP request now and let the caller
             * retry (typical "ping" pattern). */
            arp_send_request(&nexthop);
            return -2;
        }
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

    /* Accept if (1) the packet is for us, (2) it's a broadcast, OR
     * (3) we don't have an IP yet (pre-DHCP — anything that made it
     * past the Ethernet MAC filter is interesting). */
    int is_bcast   = is_ip_broadcast(&iph->dst);
    int matches_us = we_have_ip() &&
                     iph->dst.b[0] == g_my_ip.b[0] &&
                     iph->dst.b[1] == g_my_ip.b[1] &&
                     iph->dst.b[2] == g_my_ip.b[2] &&
                     iph->dst.b[3] == g_my_ip.b[3];
    if (!matches_us && !is_bcast && we_have_ip()) return;

    const uint8_t *body  = (const uint8_t *)payload + ihl;
    uint32_t       blen  = (uint32_t)ntohs(iph->total_len) - ihl;

    switch (iph->proto) {
        case IP_PROTO_ICMP: icmp_rx(eth, iph, body, blen); break;
        case IP_PROTO_TCP:  tcp_rx (iph, body, (int)blen); break;
        case IP_PROTO_UDP:  udp_rx (iph, body, (int)blen); break;
        default:                                            break;
    }
}
