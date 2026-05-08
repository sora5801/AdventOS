#include "icmp.h"
#include "ip.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

volatile int      g_ping_received  = 0;
volatile uint16_t g_ping_last_seq  = 0;
volatile uint32_t g_ping_last_tick = 0;

void icmp_rx(const struct eth_hdr *eth, const struct ip_hdr *iph,
             const void *payload, uint32_t len) {
    (void)eth;
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *ih = payload;

    /* Validate checksum over the whole ICMP message. */
    if (ip_checksum(payload, len) != 0) return;

    if (ih->type == ICMP_ECHO_REQUEST) {
        /* Echo back: same payload bytes, swap type, recompute csum. */
        uint8_t buf[1500];
        if (len > sizeof(buf)) return;
        memcpy(buf, payload, len);

        struct icmp_hdr *out = (struct icmp_hdr *)buf;
        out->type = ICMP_ECHO_REPLY;
        out->csum = 0;
        out->csum = ip_checksum(buf, len);

        ip_send(&iph->src, IP_PROTO_ICMP, buf, len);
    } else if (ih->type == ICMP_ECHO_REPLY) {
        g_ping_last_seq  = ntohs(ih->seq);
        g_ping_last_tick = pit_ticks();
        g_ping_received  = 1;
    }
}

int icmp_send_echo(const struct ip_addr *dst, uint16_t id, uint16_t seq) {
    /* 8-byte ICMP header + 32 bytes of pattern payload. */
    uint8_t buf[8 + 32];
    struct icmp_hdr *ih = (struct icmp_hdr *)buf;
    ih->type = ICMP_ECHO_REQUEST;
    ih->code = 0;
    ih->csum = 0;
    ih->id   = htons(id);
    ih->seq  = htons(seq);

    /* Filler payload: just an incrementing pattern so we can eyeball
     * the reply if we ever dump it. */
    for (int i = 0; i < 32; i++) buf[8 + i] = (uint8_t)(0x40 + i);

    ih->csum = ip_checksum(buf, sizeof(buf));

    return ip_send(dst, IP_PROTO_ICMP, buf, sizeof(buf));
}
