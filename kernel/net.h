#ifndef ADVENTOS_NET_H
#define ADVENTOS_NET_H

#include "../include/types.h"

/*
 * Shared types for the Ethernet/ARP/IPv4/ICMP stack and the NIC
 * driver beneath it.
 */

struct mac_addr { uint8_t b[6]; } __attribute__((packed));
struct ip_addr  { uint8_t b[4]; } __attribute__((packed));

/* On-the-wire byte order is big-endian. */
static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint16_t ntohs(uint16_t v) { return htons(v); }

static inline uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0xFFu)
         | ((v >> 8)  & 0xFF00u)
         | ((v << 8)  & 0xFF0000u)
         | ((v << 24) & 0xFF000000u);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

extern struct mac_addr g_my_mac;
extern struct ip_addr  g_my_ip;
extern struct ip_addr  g_gateway_ip;
extern struct ip_addr  g_subnet_mask;
extern int             g_net_up;

/* Bring up the network stack: discover NIC, init drivers, fix up IP
 * config (hard-coded for SLIRP). Safe to call before interrupts. */
void net_init(void);

/* Driver fans out received frames here. */
void net_rx_frame(const void *frame, uint32_t len);

/* Send a raw Ethernet frame via the NIC driver. */
int  net_send_frame(const void *frame, uint32_t len);

/* Decide whether `dst` is on our subnet (true) or needs to go via
 * the default gateway (false). */
int  net_is_local(const struct ip_addr *dst);

/* Pretty-print to kprintf for shell commands. */
void net_print_mac(const struct mac_addr *m);
void net_print_ip (const struct ip_addr  *ip);

#endif
