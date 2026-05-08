#ifndef ADVENTOS_IP_H
#define ADVENTOS_IP_H

#include "../include/types.h"
#include "net.h"
#include "eth.h"

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

struct ip_hdr {
    uint8_t        ver_ihl;       /* 4: version | 4: IHL (in dwords) */
    uint8_t        tos;
    uint16_t       total_len;     /* header + payload, big-endian */
    uint16_t       id;
    uint16_t       flags_frag;
    uint8_t        ttl;
    uint8_t        proto;
    uint16_t       csum;
    struct ip_addr src;
    struct ip_addr dst;
} __attribute__((packed));

uint16_t ip_checksum(const void *data, uint32_t len);

void     ip_rx(const struct eth_hdr *eth, const void *payload, uint32_t len);

/* Build + send an IPv4 packet. Resolves dst MAC via ARP (and the
 * gateway if dst is off-subnet); returns -1 on no-route. */
int      ip_send(const struct ip_addr *dst, uint8_t proto,
                 const void *payload, uint32_t len);

#endif
