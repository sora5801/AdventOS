/*
 * virtio-net — paravirtualized NIC driver. Integrates with the
 * existing eth/ip/tcp stack via net.c's pluggable backend pointer
 * (see net.h::g_nic_send_frame).
 *
 * QEMU CLI to enable:
 *   -netdev user,id=net0,hostfwd=tcp::8080-:80 \
 *   -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56
 *
 * Coexists with the RTL8139 driver: whichever returns success from
 * its init call first wins net_send_frame; if RTL8139 is present,
 * virtio-net stays idle even if probed.
 */
#ifndef ADVENTOS_VIRTIO_NET_H
#define ADVENTOS_VIRTIO_NET_H

#include "../include/types.h"
#include "net.h"

/* Probe + init. Returns 0 on success (and writes MAC into *out_mac),
 * -1 if no virtio-net device is present. Must be called after
 * pmm/paging/pit are up. */
int virtio_net_init(struct mac_addr *out_mac);

/* Synchronous send. Returns bytes sent or -1. Used internally as
 * net.c's NIC-send callback. */
int virtio_net_send(const void *frame, uint32_t len);

/* Spawn the RX polling task. Must be called AFTER task_init() runs.
 * Same lifecycle as usb_start_polling. */
void virtio_net_start_polling(void);

#endif
