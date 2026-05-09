#ifndef ADVENTOS_DHCP_H
#define ADVENTOS_DHCP_H

#include "../include/types.h"

/*
 * Tiny DHCP client.
 *
 * Runs synchronously at boot:
 *   1. Send DHCPDISCOVER (broadcast, src=0.0.0.0:68 dst=255.255.255.255:67)
 *   2. Wait up to ~2s for a DHCPOFFER
 *   3. Send DHCPREQUEST naming the offered IP + server identifier
 *   4. Wait up to ~2s for a DHCPACK
 *   5. Commit g_my_ip / g_gateway_ip / g_subnet_mask / g_dns_server
 *
 * If anything times out we fall back to SLIRP's hardcoded values
 * (10.0.2.15 / 10.0.2.2 / 255.255.255.0 / 10.0.2.3) so the rest of
 * the system still has a usable network. Returns 0 on a real lease,
 * -1 on fallback.
 */

int dhcp_acquire_lease(void);

#endif
