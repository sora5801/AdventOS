/*
 * UHCI 1.1 host controller driver — public API.
 *
 * UHCI is the simplest of the USB 1.x host controllers: an I/O-space
 * register interface, a 4 KiB frame list of 32-bit pointers, and a
 * schedule of Queue Heads (QHs) and Transfer Descriptors (TDs) the
 * controller walks once per 1 ms frame.
 *
 * QEMU's `-usb` (Intel PIIX3) exposes a UHCI controller at PCI
 * vendor 0x8086 device 0x7020. Two ports. That's what we drive.
 *
 * This module exposes synchronous transfer primitives. Higher-level
 * code (usb_core.c, usb_hid.c) builds on top.
 */
#ifndef ADVENTOS_UHCI_H
#define ADVENTOS_UHCI_H

#include "usb.h"

/* Initialize: probe PCI, take ownership from BIOS, allocate the
 * frame list + schedule QH, reset, run. Returns 0 if a controller
 * was found and brought up, -1 if no UHCI is present. */
int uhci_init(void);

/* True if uhci_init() found a controller (everything else no-ops). */
int uhci_present(void);

/* Run a SETUP/data/STATUS control transfer. Returns 0 on success
 * (USB_OK), USB_ERR_* otherwise. `data` may be NULL for zero-data
 * control requests; otherwise it's the host buffer for IN data
 * (read FROM device) or OUT data (written TO device).
 *
 *   addr        — USB device address (0 during enumeration)
 *   low_speed   — 1 if device is low-speed (1.5 Mbps)
 *   ep0_max     — endpoint-0 max packet size (8 default; later 8/16/32/64)
 *   setup       — 8-byte SETUP packet
 *   data        — buffer for data stage (or NULL)
 *   data_len    — length in bytes of data stage
 *   data_in     — direction: 1 if data goes device→host (IN)
 */
int uhci_control_transfer(uint8_t addr, int low_speed, int ep0_max,
                          const struct usb_setup_packet *setup,
                          void *data, int data_len, int data_in);

/* Single one-shot interrupt-IN transfer (for HID polling).
 * Reads up to `ep_max` bytes from endpoint `ep` of `addr`.
 *
 * Returns received byte count on success, USB_ERR_NAK if the
 * device responded NAK (no new data — common when keys haven't
 * changed), or other USB_ERR_* on failure.
 *
 * `*toggle` is the data toggle state: in/out parameter. Caller
 * keeps it across calls so DATA0/DATA1 alternates correctly. */
int uhci_int_in(uint8_t addr, int low_speed, int ep_max,
                int ep, void *buf, int max_len, int *toggle);

/* Reset+probe the root-hub ports. For each connected port, returns
 * 1 in `connected[i]` and the low-speed flag in `low_speed[i]`.
 * `connected`/`low_speed` arrays must hold at least `*n_ports`
 * entries; on return *n_ports is set to how many ports the
 * controller has (always 2 for QEMU UHCI). */
void uhci_probe_ports(int *connected, int *low_speed, int *n_ports);

#endif
