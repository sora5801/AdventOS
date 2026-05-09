#include "udp.h"
#include "string.h"
#include "kprintf.h"

struct listener {
    uint16_t    port;
    udp_recv_cb cb;
};

static struct listener g_listeners[UDP_MAX_LISTENERS];

void udp_init(void) {
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) g_listeners[i].port = 0;
}

int udp_listen(uint16_t port, udp_recv_cb cb) {
    if (port == 0) return -1;

    /* Replace existing registration for the same port. */
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (g_listeners[i].port == port) {
            g_listeners[i].cb = cb;
            return 0;
        }
    }
    /* Otherwise pick the first empty slot. */
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (g_listeners[i].port == 0) {
            g_listeners[i].port = port;
            g_listeners[i].cb   = cb;
            return 0;
        }
    }
    return -1;
}

/* Pseudo-header + UDP header + payload, all packed flat for one
 * call to ip_checksum. Same trick the TCP layer uses. */
static uint16_t udp_checksum(const struct ip_addr *src,
                             const struct ip_addr *dst,
                             const void *seg, uint16_t seg_len) {
    uint8_t buf[12 + 1500];
    if (seg_len > 1500) return 0;

    memcpy(buf + 0, src, 4);
    memcpy(buf + 4, dst, 4);
    buf[8]  = 0;
    buf[9]  = IP_PROTO_UDP;
    buf[10] = (uint8_t)(seg_len >> 8);
    buf[11] = (uint8_t)(seg_len & 0xFF);

    memcpy(buf + 12, seg, seg_len);

    uint16_t csum = ip_checksum(buf, 12u + seg_len);
    /* RFC 768: a transmitted checksum of zero means "no checksum",
     * so a real all-zero result is sent as 0xFFFF. */
    return csum == 0 ? (uint16_t)0xFFFF : csum;
}

int udp_send(const struct ip_addr *dst, uint16_t src_port,
             uint16_t dst_port, const void *data, int len) {
    if (len < 0 || len > 1500 - (int)sizeof(struct udp_hdr)) return -1;

    uint8_t buf[1500];
    struct udp_hdr *uh = (struct udp_hdr *)buf;
    uh->src_port = htons(src_port);
    uh->dst_port = htons(dst_port);
    uh->length   = htons((uint16_t)(sizeof(*uh) + len));
    uh->csum     = 0;
    if (len > 0) memcpy(buf + sizeof(*uh), data, (size_t)len);

    /* Compute checksum over (pseudo-header + uh + data). For broadcast
     * sends the src is g_my_ip which may be 0.0.0.0 pre-DHCP — that's
     * fine, the server treats 0.0.0.0 as "no client IP yet". */
    uh->csum = udp_checksum(&g_my_ip, dst, buf,
                            (uint16_t)(sizeof(*uh) + len));

    return ip_send(dst, IP_PROTO_UDP, buf, sizeof(*uh) + len);
}

void udp_rx(const struct ip_hdr *iph, const void *seg, int len) {
    if (len < (int)sizeof(struct udp_hdr)) return;
    const struct udp_hdr *uh = seg;

    uint16_t dst_port = ntohs(uh->dst_port);
    uint16_t src_port = ntohs(uh->src_port);
    uint16_t udp_len  = ntohs(uh->length);
    if (udp_len > len) return;
    if (udp_len < sizeof(*uh)) return;

    int payload_len = (int)udp_len - (int)sizeof(*uh);
    const uint8_t *payload = (const uint8_t *)seg + sizeof(*uh);

    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (g_listeners[i].port == dst_port && g_listeners[i].cb) {
            g_listeners[i].cb(&iph->src, src_port, payload, payload_len);
            return;
        }
    }
    /* No listener — silently drop. */
}
