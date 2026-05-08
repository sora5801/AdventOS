#include "arp.h"
#include "spinlock.h"
#include "string.h"
#include "kprintf.h"

#define ARP_CACHE_SIZE 16

struct arp_entry {
    int             used;
    struct ip_addr  ip;
    struct mac_addr mac;
};

static struct arp_entry g_cache[ARP_CACHE_SIZE];
static spinlock_t       g_cache_lock = SPINLOCK_INIT;

static int ip_eq(const struct ip_addr *a, const struct ip_addr *b) {
    for (int i = 0; i < 4; i++) if (a->b[i] != b->b[i]) return 0;
    return 1;
}

void arp_cache_insert(const struct ip_addr *ip, const struct mac_addr *mac) {
    spin_lock(&g_cache_lock);
    /* Replace if already present */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].used && ip_eq(&g_cache[i].ip, ip)) {
            g_cache[i].mac = *mac;
            spin_unlock(&g_cache_lock);
            return;
        }
    }
    /* Else fill first empty slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].used) {
            g_cache[i].used = 1;
            g_cache[i].ip   = *ip;
            g_cache[i].mac  = *mac;
            spin_unlock(&g_cache_lock);
            return;
        }
    }
    /* Cache full — overwrite slot 0 (no LRU policy yet). */
    g_cache[0].ip  = *ip;
    g_cache[0].mac = *mac;
    spin_unlock(&g_cache_lock);
}

int arp_lookup(const struct ip_addr *ip, struct mac_addr *out) {
    int found = 0;
    spin_lock(&g_cache_lock);
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].used && ip_eq(&g_cache[i].ip, ip)) {
            *out  = g_cache[i].mac;
            found = 1;
            break;
        }
    }
    spin_unlock(&g_cache_lock);
    return found ? 0 : -1;
}

int arp_send_request(const struct ip_addr *target) {
    struct arp_pkt p;
    p.hw_type    = htons(ARP_HW_ETHERNET);
    p.proto_type = htons(ARP_PROTO_IPV4);
    p.hw_size    = 6;
    p.proto_size = 4;
    p.opcode     = htons(ARP_OP_REQUEST);
    p.sender_mac = g_my_mac;
    p.sender_ip  = g_my_ip;
    /* In a who-has, target_mac is don't-care (often zeroed). */
    memset(&p.target_mac, 0, sizeof(p.target_mac));
    p.target_ip  = *target;

    return eth_send(&g_mac_broadcast, ETH_TYPE_ARP, &p, sizeof(p));
}

static int arp_send_reply(const struct mac_addr *to_mac,
                          const struct ip_addr  *to_ip) {
    struct arp_pkt p;
    p.hw_type    = htons(ARP_HW_ETHERNET);
    p.proto_type = htons(ARP_PROTO_IPV4);
    p.hw_size    = 6;
    p.proto_size = 4;
    p.opcode     = htons(ARP_OP_REPLY);
    p.sender_mac = g_my_mac;
    p.sender_ip  = g_my_ip;
    p.target_mac = *to_mac;
    p.target_ip  = *to_ip;

    return eth_send(to_mac, ETH_TYPE_ARP, &p, sizeof(p));
}

void arp_rx(const struct eth_hdr *eth, const void *payload, uint32_t len) {
    (void)eth;
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *p = payload;

    if (ntohs(p->hw_type)    != ARP_HW_ETHERNET) return;
    if (ntohs(p->proto_type) != ARP_PROTO_IPV4)  return;

    /* Refresh / install cache entry from the sender side regardless
     * of opcode — every ARP packet on the wire teaches us where
     * sender_ip lives. */
    arp_cache_insert(&p->sender_ip, &p->sender_mac);

    if (ntohs(p->opcode) == ARP_OP_REQUEST) {
        /* Reply only if they're asking about us. */
        int for_us = 1;
        for (int i = 0; i < 4; i++) {
            if (p->target_ip.b[i] != g_my_ip.b[i]) { for_us = 0; break; }
        }
        if (for_us) arp_send_reply(&p->sender_mac, &p->sender_ip);
    }
}

void arp_print_cache(void) {
    kputs("ARP cache:\n");
    int n = 0;
    spin_lock(&g_cache_lock);
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].used) continue;
        kputs("  ");
        net_print_ip(&g_cache[i].ip);
        kputs("  ->  ");
        net_print_mac(&g_cache[i].mac);
        kputc('\n');
        n++;
    }
    spin_unlock(&g_cache_lock);
    if (n == 0) kputs("  (empty)\n");
}
