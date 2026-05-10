/*
 * USB Mass Storage Class — Bulk-Only Transport (BOT) + a small
 * SCSI command set, sufficient to read/write a flash drive or
 * QEMU's `-device usb-storage`.
 *
 * Public API is just the attach hook called from usb_core's class
 * binding. The driver registers itself with blkdev_register() so
 * the rest of the kernel sees it as just another block device.
 */
#ifndef ADVENTOS_USB_MSC_H
#define ADVENTOS_USB_MSC_H

#include "usb.h"

/* Called from usb_core when an interface with bInterfaceClass=0x08
 * (Mass Storage), bInterfaceSubClass=0x06 (SCSI transparent),
 * bInterfaceProtocol=0x50 (BOT) is found.
 *
 *   d           — the enumerated USB device (already addressed,
 *                 configured)
 *   iface_num   — interface number (for class requests)
 *   ep_in       — bulk-IN endpoint number (1..15)
 *   ep_out      — bulk-OUT endpoint number
 *   ep_max      — bulk endpoint max-packet size (always 64 for
 *                 full-speed BOT) */
void usb_msc_attach(struct usb_device *d,
                    int iface_num, int ep_in, int ep_out, int ep_max);

#endif
