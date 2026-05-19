/*
 * USB core — enumeration glue layered on top of UHCI's transfer
 * primitives. Brings devices through the standard reset → assign
 * address → read descriptors → set configuration sequence and
 * hands them off to class drivers.
 */
#ifndef ADVENTOS_USB_CORE_H
#define ADVENTOS_USB_CORE_H

#include "usb.h"

/* Top-level: detect any UHCI controllers, probe ports, enumerate
 * attached devices, and start class-driver poll tasks. Called once
 * after PCI/IRQ are up. */
void usb_init(void);

/* Late init: starts class-driver background tasks (HID polling).
 * Must be called AFTER the rest of kernel boot (network init,
 * task system fully set up, etc.) — otherwise the polling task
 * gets dispatched onto an AP while the BSP is still running
 * single-threaded init code that hasn't taken proper SMP locks
 * around shared state. Called near the end of kmain. */
void usb_start_polling(void);

/* Convenience wrapper around uhci_control_transfer with the typical
 * GET_DESCRIPTOR phrasing. */
int usb_get_descriptor(struct usb_device *d,
                       uint8_t type, uint8_t index, uint16_t lang,
                       void *out, int max_len);

/* SET_CONFIGURATION(value). */
int usb_set_configuration(struct usb_device *d, uint8_t value);

/* HID-specific: SET_PROTOCOL(BOOT or REPORT) and SET_IDLE(0). */
int usb_hid_set_protocol(struct usb_device *d, int interface, int protocol);
int usb_hid_set_idle    (struct usb_device *d, int interface, int duration_4ms);

/* Enumerate a device that's already had its USB port reset and is
 * speaking at the default address 0. Called twice in this codebase:
 *
 *   - From usb_init() for each connected root-hub port (uhci_probe_ports
 *     does the reset, then this fires).
 *   - From usb_hub_attach() for each port behind a USB hub once the
 *     hub driver has done its class-specific port reset.
 *
 * `low_speed` is the speed of the freshly-reset device (1 for 1.5 Mbps
 * low-speed, 0 for 12 Mbps full-speed). `origin` is a human-readable
 * tag for logging — "port 1" or "hub 2 port 3", etc.
 *
 * `hc` selects the host controller this device lives on; every
 * subsequent transfer for the device dispatches through hc->* fnptrs. */
struct usb_hc_ops;
void usb_enumerate_default(int low_speed, const char *origin,
                           const struct usb_hc_ops *hc);

#endif
