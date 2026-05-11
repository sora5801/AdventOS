#ifndef ADVENTOS_DNS_H
#define ADVENTOS_DNS_H

#include "../include/types.h"
#include "net.h"

/*
 * Tiny synchronous DNS resolver (A records only).
 *
 * dns_resolve(name, &out) builds an A-record query, sends it to
 * g_dns_server:53, busy-waits up to ~timeout_ms for the response,
 * parses out the first A record, and writes the IPv4 address into
 * *out. Returns 0 on success, -1 on timeout / parse fail / DNS not
 * configured.
 *
 * No caching. No CNAME chasing (a CNAME-in-the-answer is followed
 * one step if a subsequent A record references it; if not, we fail).
 * No EDNS, no DNSSEC, no recursion control. The DNS server is
 * expected to be a recursive resolver — that's what SLIRP gives us
 * (10.0.2.3 forwards to the host's resolver).
 */

void dns_init(void);

int  dns_resolve(const char *name, struct ip_addr *out);

/* Session 60 — parse /etc/resolv.conf at boot.  Each `nameserver IP`
 * line adds the listed server to a small fail-over list (max
 * DNS_SERVERS_MAX).  Without /etc/resolv.conf, dns_resolve falls back
 * to whatever DHCP option 6 set.  Called from net_init after the
 * filesystem is mounted. */
void dns_load_resolv_conf(void);

/* TTL-based cache stats — fills out[0..3] = lookups, hits, misses,
 * live entries.  Used by [t43]. */
void dns_cache_stats(uint32_t out[4]);

/* Snapshot the configured DNS-server list as a flat array; out[i]
 * holds the i-th server's 4 bytes.  Returns how many are populated.
 * Used by [t43]'s introspection check. */
int  dns_get_servers(struct ip_addr *out, int max);

#endif
