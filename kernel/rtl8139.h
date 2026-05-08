#ifndef ADVENTOS_RTL8139_H
#define ADVENTOS_RTL8139_H

#include "../include/types.h"
#include "net.h"

/*
 * RTL8139 NIC driver. Discovers the card via PCI, sets up an 8 KiB
 * RX ring (no-wrap mode, +16 byte WRAP padding plus max packet),
 * uses 4 round-robin TX descriptors, drives RX off IRQ 11.
 *
 * Returns 0 on success and writes the discovered MAC into *out_mac.
 * Returns -1 if no RTL8139 is present.
 */
int  rtl8139_init(struct mac_addr *out_mac);

/* Synchronously enqueue and start a TX. Returns bytes sent or -1. */
int  rtl8139_send(const void *frame, uint32_t len);

#endif
