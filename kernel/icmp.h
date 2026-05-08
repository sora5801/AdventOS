#ifndef ADVENTOS_ICMP_H
#define ADVENTOS_ICMP_H

#include "../include/types.h"
#include "ip.h"

#define ICMP_ECHO_REPLY    0
#define ICMP_ECHO_REQUEST  8

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t csum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

void icmp_rx(const struct eth_hdr *eth, const struct ip_hdr *iph,
             const void *payload, uint32_t len);

int  icmp_send_echo(const struct ip_addr *dst,
                    uint16_t id, uint16_t seq);

/* Last echo-reply rendezvous, set by icmp_rx. Cleared by ping cmd
 * before sending the next request. Volatile because it crosses
 * IRQ-context / shell-context. */
extern volatile int      g_ping_received;
extern volatile uint16_t g_ping_last_seq;
extern volatile uint32_t g_ping_last_tick;

#endif
