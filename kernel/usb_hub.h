/*
 * USB Hub class driver — session 44.
 *
 * USB 1.1 §11.  When usb_core sees a device with bDeviceClass=0x09
 * it routes to usb_hub_attach below. The hub driver fetches the
 * hub class descriptor (different tag than the standard descriptors;
 * accessed via a class-typed GET_DESCRIPTOR), powers each downstream
 * port via SET_PORT_FEATURE(PORT_POWER), polls GET_PORT_STATUS for
 * connect, and on connect drives the port reset sequence + asks
 * usb_core to re-enumerate the device behind the port.
 *
 * One hub depth is enough for the demo. Real systems support nested
 * hubs (USB allows up to 5 levels); the recursive call into
 * usb_enumerate_default would Just Work, but we don't test it.
 */
#ifndef ADVENTOS_USB_HUB_H
#define ADVENTOS_USB_HUB_H

#include "usb.h"

/* Called from usb_core when a class-9 device is enumerated.
 * Fetches the hub descriptor, then walks the ports — for each
 * connected port, resets it and calls back into usb_core to
 * enumerate the device behind. */
void usb_hub_attach(struct usb_device *d);

#endif
