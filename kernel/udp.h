#ifndef ADVENTOS_UDP_H
#define ADVENTOS_UDP_H

#include "../include/types.h"
#include "net.h"
#include "ip.h"

/*
 * UDP. Connectionless, stateless. Each port can have one in-kernel
 * listener registered via udp_listen(); the listener is invoked
 * directly from the IRQ-context UDP rx path.
 *
 * Pseudo-header checksum is computed/verified by reusing the same
 * "build a flat buffer, run ip_checksum" trick the TCP layer uses —
 * keeps us out of the packed-struct alignment minefield.
 */

#define UDP_MAX_LISTENERS  4

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;       /* header + data, in host order set by us */
    uint16_t csum;         /* 0 = no-checksum (allowed for IPv4) */
} __attribute__((packed));

typedef void (*udp_recv_cb)(const struct ip_addr *src_ip,
                            uint16_t              src_port,
                            const void           *data,
                            int                   len);

void udp_init(void);

/* Register a callback for incoming UDP datagrams on `port`. Returns
 * 0 on success, -1 if the table is full. Subsequent registrations on
 * the same port replace the old callback. */
int  udp_listen(uint16_t port, udp_recv_cb cb);

/* Send `data` to `dst:dst_port` from `src_port`. Returns 0 on success,
 * negative on failure. dst can be a unicast or 255.255.255.255. */
int  udp_send  (const struct ip_addr *dst, uint16_t src_port,
                uint16_t dst_port, const void *data, int len);

/* Called by ip_rx when protocol == 17. */
void udp_rx    (const struct ip_hdr *iph, const void *seg, int len);

#endif
