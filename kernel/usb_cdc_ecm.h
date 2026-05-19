/*
 * USB CDC-ECM (Communications Device Class — Ethernet Networking
 * Control Model). The standard way modern USB-Ethernet dongles
 * speak (e.g. Apple's official USB-C Ethernet adapter, Belkin USB
 * Ethernet, many cheap travel adapters). Distinct from RNDIS
 * (Microsoft proprietary) and AX88179 (vendor-specific).
 *
 * Wire model:
 *
 *   - Comm interface (class 0x02, subclass 0x06 ECM): an interrupt-IN
 *     endpoint that delivers connection-status notifications
 *     (NETWORK_CONNECTION, CONNECTION_SPEED_CHANGE). Optional —
 *     we don't act on them in this minimal driver.
 *
 *   - Data interface (class 0x0A): TWO alternate settings. Alt 0
 *     has no endpoints (the link is "logically down"). Alt 1 has
 *     the bulk-IN + bulk-OUT endpoints. Drivers MUST issue
 *     SET_INTERFACE(data_iface, alt=1) before any data flows —
 *     this is the ECM equivalent of "ifconfig up."
 *
 *   - Frame format: raw Ethernet on the wire. One bulk transfer
 *     per frame. Short packet (or ZLP if frame size == ep_max)
 *     terminates the transfer.
 *
 *   - MAC address: advertised in the Ethernet Functional Descriptor
 *     (subtype 0x0F, byte 3 = iMACAddress string-descriptor index).
 *     We fetch the string descriptor and parse 12 hex digits.
 *
 * QEMU CLI:
 *   -device usb-net,netdev=net0,mac=52:54:00:12:34:78 \
 *   -netdev user,id=net0,...
 *
 * AdventOS slots this in next to e1000 / rtl8139 / virtio-net via
 * net_attach_nic — the link-layer is identical (raw Ethernet), the
 * only thing that differs is the transport beneath it (USB bulk
 * pipes vs PIO / MMIO).
 */
#ifndef ADVENTOS_USB_CDC_ECM_H
#define ADVENTOS_USB_CDC_ECM_H

#include "usb.h"
#include "../include/types.h"

struct mac_addr;

/* Called from usb_core's enumeration once a CDC Ethernet data
 * interface (subclass 0x06) is identified. `data_iface_alt1` is the
 * alternate setting that has the bulk endpoints (we send
 * SET_INTERFACE before any I/O). `imac_str_idx` is the string-
 * descriptor index that holds the 12-hex-digit MAC. */
void usb_cdc_ecm_attach(struct usb_device *d,
                        int comm_iface, int data_iface,
                        int data_iface_alt1,
                        int ep_in, int ep_out, int ep_max,
                        int imac_str_idx);

/* NIC backend: called from net_init like e1000_init / rtl8139_init.
 * Returns 0 if a CDC-ECM device is bound (fills MAC), -1 otherwise. */
int  usb_cdc_ecm_init(struct mac_addr *out_mac);

/* Transmit one Ethernet frame. Signature matches nic_send_fn in net.c. */
int  usb_cdc_ecm_send(const void *frame, uint32_t len);

/* Spawn the RX polling task. Called from usb_start_polling alongside
 * the HID + CDC-ACM polling tasks. */
void usb_cdc_ecm_start_polling(void);

#endif
