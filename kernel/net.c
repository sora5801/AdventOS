#include "net.h"
#include "eth.h"
#include "rtl8139.h"
#include "virtio_net.h"
#include "e1000.h"
#include "usb_cdc_ecm.h"
#include "kprintf.h"

struct mac_addr g_my_mac;
/* All-zero defaults — DHCP fills these in at boot. If DHCP times out
 * the dhcp layer falls back to SLIRP's hardcoded values. */
struct ip_addr  g_my_ip       = { { 0, 0, 0, 0 } };
struct ip_addr  g_gateway_ip  = { { 0, 0, 0, 0 } };
struct ip_addr  g_subnet_mask = { { 0, 0, 0, 0 } };
struct ip_addr  g_dns_server  = { { 0, 0, 0, 0 } };
int             g_net_up;

/* Which NIC backend is live. Set by net_init at boot; net_send_frame
 * dispatches through this pointer. */
typedef int (*nic_send_fn)(const void *frame, uint32_t len);
static nic_send_fn g_nic_send;

void net_init(void) {
    /* Try RTL8139 first — it's the legacy default and gets first
     * crack at IRQ 11. */
    if (rtl8139_init(&g_my_mac) == 0) {
        g_nic_send = rtl8139_send;
        g_net_up = 1;
        kputs("net: link up (rtl8139) — MAC ");
        net_print_mac(&g_my_mac);
        kputs("  (IP unconfigured — waiting for DHCP)\n");
        return;
    }

    /* Fall back to virtio-net for modern QEMU configurations. */
    if (virtio_net_init(&g_my_mac) == 0) {
        g_nic_send = virtio_net_send;
        g_net_up = 1;
        kputs("net: link up (virtio-net) — MAC ");
        net_print_mac(&g_my_mac);
        kputs("  (IP unconfigured — waiting for DHCP)\n");
        return;
    }

    /* Intel 82540EM / 82574L (e1000 / e1000e). The chip that ships on
     * a lot of real-hardware boards. */
    if (e1000_init(&g_my_mac) == 0) {
        g_nic_send = e1000_send;
        g_net_up = 1;
        kputs("net: link up (e1000) — MAC ");
        net_print_mac(&g_my_mac);
        kputs("  (IP unconfigured — waiting for DHCP)\n");
        return;
    }

    /* Last resort: USB CDC-ECM. usb_init() must have run before us
     * for the device to be enumerated; the boot ordering in kmain.c
     * guarantees that. */
    if (usb_cdc_ecm_init(&g_my_mac) == 0) {
        g_nic_send = usb_cdc_ecm_send;
        g_net_up = 1;
        kputs("net: link up (usb-cdc-ecm) — MAC ");
        net_print_mac(&g_my_mac);
        kputs("  (IP unconfigured — waiting for DHCP)\n");
        return;
    }

    kputs("net: no NIC found (tried rtl8139, virtio-net, e1000, cdc-ecm) — networking offline\n");
}

void net_rx_frame(const void *frame, uint32_t len) {
    eth_rx(frame, len);
}

int net_send_frame(const void *frame, uint32_t len) {
    if (!g_net_up || !g_nic_send) return -1;
    return g_nic_send(frame, len);
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
