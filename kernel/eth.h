#ifndef ADVENTOS_ETH_H
#define ADVENTOS_ETH_H

#include "../include/types.h"
#include "net.h"

#define ETH_TYPE_IPV4  0x0800
#define ETH_TYPE_ARP   0x0806

#define ETH_ALEN       6
#define ETH_FRAME_MAX  1518   /* incl. 14B header, 1500 payload, 4B FCS */
#define ETH_MTU        1500

struct eth_hdr {
    struct mac_addr dst;
    struct mac_addr src;
    uint16_t        ethertype;   /* big-endian on the wire */
} __attribute__((packed));

extern const struct mac_addr g_mac_broadcast;

void eth_rx(const void *frame, uint32_t len);

/* Build + send an Ethernet frame with the given dst MAC, ethertype,
 * and payload. Returns 0 on success. */
int  eth_send(const struct mac_addr *dst, uint16_t ethertype,
              const void *payload, uint32_t len);

#endif
