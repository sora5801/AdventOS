#ifndef ADVENTOS_ARP_H
#define ADVENTOS_ARP_H

#include "../include/types.h"
#include "net.h"
#include "eth.h"

#define ARP_HW_ETHERNET   1
#define ARP_PROTO_IPV4    0x0800
#define ARP_OP_REQUEST    1
#define ARP_OP_REPLY      2

struct arp_pkt {
    uint16_t        hw_type;
    uint16_t        proto_type;
    uint8_t         hw_size;
    uint8_t         proto_size;
    uint16_t        opcode;
    struct mac_addr sender_mac;
    struct ip_addr  sender_ip;
    struct mac_addr target_mac;
    struct ip_addr  target_ip;
} __attribute__((packed));

/* Receive an ARP packet. May install/refresh a cache entry and may
 * fire a reply if it's a request for our IP. */
void arp_rx(const struct eth_hdr *eth, const void *payload, uint32_t len);

/* Broadcast a who-has request for the given IP. */
int  arp_send_request(const struct ip_addr *target);

/* Resolve `ip` to a MAC by checking the cache. Returns 0 on hit and
 * fills *out, or -1 on miss. */
int  arp_lookup(const struct ip_addr *ip, struct mac_addr *out);

/* Hand-install a cache entry (used for sanity, also from ARP rx). */
void arp_cache_insert(const struct ip_addr *ip, const struct mac_addr *mac);

void arp_print_cache(void);

#endif
