/*
 * USB host-controller abstraction.
 *
 * Lets the class drivers (HID, MSC, CDC-ACM, CDC-ECM, hub) speak
 * controller-neutral transfer ops instead of calling uhci_* directly.
 * Each struct usb_device carries a `const struct usb_hc_ops *hc`
 * pointing at the controller it lives on; transfers dispatch through
 * that vtable.
 *
 * UHCI exports `g_uhci_hc_ops`; EHCI (session 126) exports
 * `g_ehci_hc_ops`. Adding a third HC (xHCI / OHCI) is one more
 * struct.
 */
#ifndef ADVENTOS_USB_HC_H
#define ADVENTOS_USB_HC_H

#include "usb.h"

struct usb_setup_packet;

struct usb_hc_ops {
    /* control_transfer: SETUP + (DATA stage) + STATUS. setup is
     * always 8 bytes. data may be NULL for zero-data control
     * requests. data_in: 1 if DATA stage is device→host. */
    int (*control_transfer)(uint8_t addr, int low_speed, int ep0_max,
                            const struct usb_setup_packet *setup,
                            void *data, int data_len, int data_in);

    /* int_in: single one-shot interrupt-IN transfer (HID polls). */
    int (*int_in)(uint8_t addr, int low_speed, int ep_max,
                  int ep, void *buf, int max_len, int *toggle);

    /* bulk_in / bulk_out: split data into ep_max-sized TDs/qTDs,
     * track per-endpoint toggle in *toggle. */
    int (*bulk_in) (uint8_t addr, int ep_max, int ep,
                    void *buf, int len, int *toggle);
    int (*bulk_out)(uint8_t addr, int ep_max, int ep,
                    const void *buf, int len, int *toggle);
};

/* The two concrete controllers' op tables. */
extern const struct usb_hc_ops g_uhci_hc_ops;
extern const struct usb_hc_ops g_ehci_hc_ops;

/* Convenience wrappers: dispatch through a usb_device's hc. */
static inline int usb_hc_control(struct usb_device *d,
                                 const struct usb_setup_packet *s,
                                 void *data, int len, int in)
{
    return d->hc->control_transfer(d->addr, d->low_speed,
                                   d->ep0_max_packet, s, data, len, in);
}

#endif
