#include "eth.h"
#include "arp.h"
#include "ip.h"
#include "string.h"
#include "kprintf.h"

const struct mac_addr g_mac_broadcast = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };

void eth_rx(const void *frame, uint32_t len) {
    if (len < sizeof(struct eth_hdr)) return;
    const struct eth_hdr *eh = frame;
    uint16_t et = ntohs(eh->ethertype);
    const uint8_t *payload = (const uint8_t *)frame + sizeof(*eh);
    uint32_t plen = len - sizeof(*eh);

    switch (et) {
        case ETH_TYPE_ARP:  arp_rx(eh, payload, plen); break;
        case ETH_TYPE_IPV4: ip_rx (eh, payload, plen); break;
        default:                                       break;   /* drop */
    }
}

int eth_send(const struct mac_addr *dst, uint16_t ethertype,
             const void *payload, uint32_t len) {
    if (len > ETH_MTU) return -1;

    /* Stack-allocated frame buffer — Ethernet max is 1518 bytes; we
     * need at most 14 (hdr) + 1500 (payload). */
    uint8_t buf[14 + ETH_MTU];
    struct eth_hdr *eh = (struct eth_hdr *)buf;
    eh->dst       = *dst;
    eh->src       = g_my_mac;
    eh->ethertype = htons(ethertype);
    memcpy(buf + sizeof(*eh), payload, len);

    return net_send_frame(buf, sizeof(*eh) + len) >= 0 ? 0 : -1;
}
