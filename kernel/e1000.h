/*
 * Intel 82540EM NIC driver (a.k.a. "e1000"). The widely-emulated
 * gigabit Ethernet card in QEMU's `-device e1000` and the chip many
 * real laptops actually ship.
 *
 * Selected from net_init() as a third NIC backend, after RTL8139
 * (legacy/cheap) and virtio-net (paravirt).  Same g_nic_send pluggable
 * interface — same eth/ip/tcp/udp/dhcp stack on top.
 *
 * QEMU CLI (replaces the rtl8139 device):
 *   -netdev user,id=net0,hostfwd=tcp::8080-:80 \
 *   -device e1000,netdev=net0,mac=52:54:00:12:34:56
 *
 * Implementation footprint: MMIO BAR0 (128 KiB register file mapped
 * into kernel virt = phys via paging_map), 16-entry RX + 16-entry TX
 * descriptor rings allocated from kmalloc, IRQ-driven completion.
 */
#ifndef ADVENTOS_E1000_H
#define ADVENTOS_E1000_H

#include "../include/types.h"
#include "net.h"

/* Probe + init.  Returns 0 + writes the MAC into *out_mac on success,
 * -1 if no Intel 82540EM (or compatible variant we recognize) is
 * present on the PCI bus. */
int e1000_init(struct mac_addr *out_mac);

/* Synchronous send.  Returns bytes sent or -1.  Used internally as
 * net.c's NIC-send callback. */
int e1000_send(const void *frame, uint32_t len);

#endif
