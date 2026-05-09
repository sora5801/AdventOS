#include "net.h"
#include "eth.h"
#include "rtl8139.h"
#include "kprintf.h"

struct mac_addr g_my_mac;
/* All-zero defaults — DHCP fills these in at boot. If DHCP times out
 * the dhcp layer falls back to SLIRP's hardcoded values. */
struct ip_addr  g_my_ip       = { { 0, 0, 0, 0 } };
struct ip_addr  g_gateway_ip  = { { 0, 0, 0, 0 } };
struct ip_addr  g_subnet_mask = { { 0, 0, 0, 0 } };
struct ip_addr  g_dns_server  = { { 0, 0, 0, 0 } };
int             g_net_up;

void net_init(void) {
    if (rtl8139_init(&g_my_mac) != 0) {
        kputs("net: no RTL8139 found, networking offline\n");
        return;
    }
    g_net_up = 1;

    kputs("net: link up — MAC ");
    net_print_mac(&g_my_mac);
    kputs("  (IP unconfigured — waiting for DHCP)\n");
}

void net_rx_frame(const void *frame, uint32_t len) {
    eth_rx(frame, len);
}

int net_send_frame(const void *frame, uint32_t len) {
    if (!g_net_up) return -1;
    return rtl8139_send(frame, len);
}

int net_is_local(const struct ip_addr *dst) {
    for (int i = 0; i < 4; i++) {
        if ((dst->b[i] & g_subnet_mask.b[i]) !=
            (g_my_ip.b[i] & g_subnet_mask.b[i])) return 0;
    }
    return 1;
}

void net_print_mac(const struct mac_addr *m) {
    kprintf("%02x:%02x:%02x:%02x:%02x:%02x",
            m->b[0], m->b[1], m->b[2], m->b[3], m->b[4], m->b[5]);
}

void net_print_ip(const struct ip_addr *ip) {
    kprintf("%u.%u.%u.%u", ip->b[0], ip->b[1], ip->b[2], ip->b[3]);
}
