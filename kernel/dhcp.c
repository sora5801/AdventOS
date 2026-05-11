#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "arp.h"
#include "pit.h"
#include "rtc.h"
#include "string.h"
#include "kprintf.h"
#include "syscall.h"     /* struct sys_dhcp_info */

/*
 * BOOTP / DHCP packet format. The fixed-size part is 240 bytes (up
 * to and including the 4-byte magic cookie); options follow.
 */
struct dhcp_pkt {
    uint8_t  op;            /* 1 = BOOTREQUEST, 2 = BOOTREPLY */
    uint8_t  htype;         /* 1 = ethernet */
    uint8_t  hlen;          /* 6 */
    uint8_t  hops;
    uint32_t xid;           /* transaction ID */
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;        /* client IP, set if already bound */
    uint32_t yiaddr;        /* "your" IP — server fills in OFFER/ACK */
    uint32_t siaddr;        /* server IP */
    uint32_t giaddr;        /* gateway/relay */
    uint8_t  chaddr[16];    /* client MAC (first 6 used) */
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;         /* 0x63825363 in network order */
    uint8_t  options[312];
} __attribute__((packed));

#define DHCP_MAGIC          0x63825363u

#define DHCP_OP_REQUEST     1
#define DHCP_OP_REPLY       2

#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS_SERVER     6
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_LIST     55
#define DHCP_OPT_END            255

#define DHCP_MSG_DISCOVER       1
#define DHCP_MSG_OFFER          2
#define DHCP_MSG_REQUEST        3
#define DHCP_MSG_ACK            5

/* State updated by the UDP rx callback so the boot path can
 * synchronously poll. */
static volatile int        g_state;       /* 0 idle, 1 got OFFER, 2 got ACK */
static struct ip_addr      g_offered_ip;
static struct ip_addr      g_offered_mask;
static struct ip_addr      g_offered_gw;
static struct ip_addr      g_offered_dns;
static struct ip_addr      g_server_id;
static uint32_t            g_xid;

/* Session 60 — lease bookkeeping for SYS_DHCP_INFO. Populated when
 * the server's ACK lands; visible to userspace via dhcp_get_info(). */
static uint32_t            g_lease_seconds;
static uint32_t            g_acquired_epoch;

#define DHCP_S_IDLE   0
#define DHCP_S_OFFER  1
#define DHCP_S_ACK    2

static int parse_options(const uint8_t *opts, int len, int *msg_type) {
    int i = 0;
    *msg_type = 0;
    while (i < len) {
        uint8_t code = opts[i++];
        if (code == DHCP_OPT_PAD) continue;
        if (code == DHCP_OPT_END) break;
        if (i >= len) break;
        uint8_t l = opts[i++];
        if (i + l > len) break;
        const uint8_t *v = opts + i;

        switch (code) {
            case DHCP_OPT_MSG_TYPE:
                if (l == 1) *msg_type = v[0];
                break;
            case DHCP_OPT_SUBNET_MASK:
                if (l == 4) for (int k = 0; k < 4; k++) g_offered_mask.b[k] = v[k];
                break;
            case DHCP_OPT_ROUTER:
                if (l >= 4) for (int k = 0; k < 4; k++) g_offered_gw.b[k]   = v[k];
                break;
            case DHCP_OPT_DNS_SERVER:
                if (l >= 4) for (int k = 0; k < 4; k++) g_offered_dns.b[k]  = v[k];
                break;
            case DHCP_OPT_SERVER_ID:
                if (l == 4) for (int k = 0; k < 4; k++) g_server_id.b[k]    = v[k];
                break;
            case DHCP_OPT_LEASE_TIME:
                /* RFC 2132 §9.2: 32-bit unsigned seconds, big-endian. */
                if (l == 4) {
                    g_lease_seconds = ((uint32_t)v[0] << 24) |
                                       ((uint32_t)v[1] << 16) |
                                       ((uint32_t)v[2] <<  8) | v[3];
                }
                break;
        }
        i += l;
    }
    return 0;
}

/* UDP listener callback for port 68 (BOOTP client). Runs in IRQ
 * context. Updates the volatile flags the boot path is polling on. */
static void on_dhcp(const struct ip_addr *src, uint16_t src_port,
                    const void *data, int len) {
    (void)src; (void)src_port;
    if (len < (int)(sizeof(struct dhcp_pkt) - 312)) return;
    const struct dhcp_pkt *p = data;
    if (p->op != DHCP_OP_REPLY)        return;
    if (ntohl(p->xid)    != g_xid)     return;
    if (ntohl(p->magic)  != DHCP_MAGIC) return;

    /* Pull yiaddr (the server's offered IP) and parse options. */
    uint32_t y = ntohl(p->yiaddr);
    g_offered_ip.b[0] = (uint8_t)(y >> 24);
    g_offered_ip.b[1] = (uint8_t)(y >> 16);
    g_offered_ip.b[2] = (uint8_t)(y >> 8);
    g_offered_ip.b[3] = (uint8_t)y;

    int opts_len = len - (int)((const uint8_t *)&p->options - (const uint8_t *)p);
    if (opts_len > (int)sizeof(p->options)) opts_len = sizeof(p->options);
    int msg = 0;
    parse_options(p->options, opts_len, &msg);

    if (msg == DHCP_MSG_OFFER) g_state = DHCP_S_OFFER;
    if (msg == DHCP_MSG_ACK)   g_state = DHCP_S_ACK;
}

static uint8_t *put_opt(uint8_t *p, uint8_t code, uint8_t len, const void *val) {
    *p++ = code;
    *p++ = len;
    if (len > 0) {
        const uint8_t *v = val;
        for (uint8_t i = 0; i < len; i++) *p++ = v[i];
    }
    return p;
}

static int send_dhcp(uint8_t msg_type, const struct ip_addr *requested_ip) {
    struct dhcp_pkt pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op    = DHCP_OP_REQUEST;
    pkt.htype = 1;
    pkt.hlen  = 6;
    pkt.xid   = htonl(g_xid);
    pkt.flags = htons(0x8000);             /* "broadcast" bit so server replies bcast */
    for (int i = 0; i < 6; i++) pkt.chaddr[i] = g_my_mac.b[i];
    pkt.magic = htonl(DHCP_MAGIC);

    uint8_t *o = pkt.options;
    o = put_opt(o, DHCP_OPT_MSG_TYPE, 1, &msg_type);
    if (msg_type == DHCP_MSG_REQUEST) {
        if (requested_ip) o = put_opt(o, DHCP_OPT_REQUESTED_IP, 4, requested_ip);
        if (g_server_id.b[0] || g_server_id.b[1] ||
            g_server_id.b[2] || g_server_id.b[3]) {
            o = put_opt(o, DHCP_OPT_SERVER_ID, 4, &g_server_id);
        }
    }
    /* Ask for the options we care about. */
    static const uint8_t param_list[] = {
        DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS_SERVER,
        DHCP_OPT_LEASE_TIME, DHCP_OPT_SERVER_ID,
    };
    o = put_opt(o, DHCP_OPT_PARAM_LIST, sizeof(param_list), param_list);
    *o++ = DHCP_OPT_END;

    int total = (int)((const uint8_t *)o - (const uint8_t *)&pkt);

    static const struct ip_addr bcast = {{255,255,255,255}};
    return udp_send(&bcast, 68, 67, &pkt, total);
}

/* Block up to ~timeout_ms waiting for g_state to reach `target`. */
static int wait_state(int target, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        if (g_state == target) return 0;
        pit_sleep(20);
        waited += 20;
    }
    return -1;
}

static void apply_fallback(void) {
    /* SLIRP defaults — what we used to hardcode. */
    g_my_ip.b[0]       = 10; g_my_ip.b[1]       = 0; g_my_ip.b[2]       = 2; g_my_ip.b[3]       = 15;
    g_gateway_ip.b[0]  = 10; g_gateway_ip.b[1]  = 0; g_gateway_ip.b[2]  = 2; g_gateway_ip.b[3]  = 2;
    g_subnet_mask.b[0] = 255;g_subnet_mask.b[1] = 255;g_subnet_mask.b[2]= 255;g_subnet_mask.b[3]= 0;
    g_dns_server.b[0]  = 10; g_dns_server.b[1]  = 0; g_dns_server.b[2]  = 2; g_dns_server.b[3]  = 3;
}

int dhcp_acquire_lease(void) {
    if (!g_net_up) return -1;

    /* Pick a transaction ID. PIT ticks scaled is good enough as
     * "random-ish" — a real client uses a CSPRNG. */
    g_xid    = (uint32_t)pit_ticks() * 2654435761u;
    g_state  = DHCP_S_IDLE;

    udp_listen(68, on_dhcp);

    kputs("dhcp: DISCOVER ... ");
    if (send_dhcp(DHCP_MSG_DISCOVER, NULL) < 0) {
        kputs("send failed; using SLIRP defaults\n");
        apply_fallback();
        return -1;
    }
    if (wait_state(DHCP_S_OFFER, 2000) < 0) {
        kputs("no OFFER within 2s; using SLIRP defaults\n");
        apply_fallback();
        return -1;
    }
    kputs("got OFFER ");
    net_print_ip(&g_offered_ip);
    kputs(", REQUEST ... ");

    if (send_dhcp(DHCP_MSG_REQUEST, &g_offered_ip) < 0) {
        kputs("send failed; using SLIRP defaults\n");
        apply_fallback();
        return -1;
    }
    if (wait_state(DHCP_S_ACK, 2000) < 0) {
        kputs("no ACK within 2s; using SLIRP defaults\n");
        apply_fallback();
        return -1;
    }

    /* Commit the lease. yiaddr was filled by the OFFER (and the ACK
     * normally repeats it; we trust the OFFER value). */
    g_my_ip       = g_offered_ip;
    g_subnet_mask = g_offered_mask;
    g_gateway_ip  = g_offered_gw;
    g_dns_server  = g_offered_dns;
    /* Session 60: stamp the lease-acquired time. parse_options set
     * g_lease_seconds when DHCP_OPT_LEASE_TIME arrived; if the server
     * didn't send one (SLIRP doesn't), assume 1 day. */
    if (g_lease_seconds == 0) g_lease_seconds = 86400;
    g_acquired_epoch = rtc_epoch_corrected();

    kputs("ACK\n");
    kputs("net: ");
    net_print_ip(&g_my_ip);  kputs("/");
    net_print_ip(&g_subnet_mask); kputs("  GW ");
    net_print_ip(&g_gateway_ip);  kputs("  DNS ");
    net_print_ip(&g_dns_server);
    kprintf("  lease=%us  (T1=%us)\n",
            (unsigned)g_lease_seconds, (unsigned)g_lease_seconds / 2);

    /* Prime the ARP cache for the gateway so the first outbound TCP
     * reply doesn't get dropped because of a cache miss (we have no
     * TCP retransmit, so a single missed SYN-ACK = stuck connection).
     * Send an ARP request and give the reply a moment to come back. */
    arp_send_request(&g_gateway_ip);
    pit_sleep(50);

    return 0;
}

/* Session 60 — flat snapshot of the lease state for userspace
 * introspection (SYS_DHCP_INFO).  Safe to call any time; if we don't
 * have a lease yet, `have_lease` is 0 and the rest is zero. */
void dhcp_get_info(struct sys_dhcp_info *out) {
    if (!out) return;
    for (int i = 0; i < 4; i++) {
        out->ip[i]         = g_my_ip.b[i];
        out->netmask[i]    = g_subnet_mask.b[i];
        out->gateway[i]    = g_gateway_ip.b[i];
        out->dns_server[i] = g_dns_server.b[i];
    }
    out->lease_seconds   = g_lease_seconds;
    out->acquired_epoch  = g_acquired_epoch;
    out->t1_renew_at     = g_lease_seconds
                           ? g_acquired_epoch + g_lease_seconds / 2
                           : 0;
    out->have_lease      = (g_state == DHCP_S_ACK) ? 1 : 0;
}
