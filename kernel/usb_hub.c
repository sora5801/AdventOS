/*
 * USB Hub class driver (USB 1.1 §11).
 *
 * Class-9 devices export a single per-device entity ("the hub") with
 * N downstream ports. To enumerate devices behind the hub we:
 *
 *   1. GET_DESCRIPTOR(0x29) — the class-specific Hub Descriptor —
 *      gives us bNbrPorts (1..255 in spec, ≤8 for QEMU's usb-hub)
 *      and bPwrOn2PwrGood (the spin time in 2 ms units between
 *      SET_PORT_FEATURE(POWER) and "the port is actually energized").
 *
 *   2. For each downstream port p in 1..bNbrPorts:
 *        SET_PORT_FEATURE(PORT_POWER, p)
 *        wait bPwrOn2PwrGood × 2 ms
 *        GET_PORT_STATUS(p) → if PORT_CONNECTION set:
 *          SET_PORT_FEATURE(PORT_RESET, p)
 *          wait 50 ms
 *          poll GET_PORT_STATUS(p) until PORT_RESET clears and
 *            PORT_ENABLE goes high (or timeout)
 *          read PORT_LOW_SPEED bit to decide bus speed
 *          usb_enumerate_default(low_speed, "hub N port M")
 *
 * USB 1.1 §11 §11.5.1.5 spells out the port-reset state machine;
 * we implement the boot-time-only case (no hot-plug after init,
 * no port-disable / suspend / resume).
 *
 * Class request encodings (USB 1.1 §11.16):
 *   bmRequestType for hub-as-a-whole : USB_DIR_* | USB_TYPE_CLASS | USB_RECIP_DEVICE
 *   bmRequestType for a single port   : USB_DIR_* | USB_TYPE_CLASS | USB_RECIP_OTHER
 *   bRequest CLEAR_FEATURE = 0x01
 *   bRequest SET_FEATURE   = 0x03
 *   bRequest GET_STATUS    = 0x00
 *   bRequest GET_DESCRIPTOR= 0x06    (with wValue = 0x29 << 8 for class desc)
 *
 * Port-feature selectors (wValue) §11.16.2.2:
 *   PORT_CONNECTION         0
 *   PORT_ENABLE             1
 *   PORT_SUSPEND            2
 *   PORT_OVER_CURRENT       3
 *   PORT_RESET              4
 *   PORT_POWER              8
 *   PORT_LOW_SPEED          9
 *   C_PORT_CONNECTION      16
 *   C_PORT_ENABLE          17
 *   C_PORT_SUSPEND         18
 *   C_PORT_OVER_CURRENT    19
 *   C_PORT_RESET           20
 *
 * Port-status bitmap (returned by GET_STATUS) §11.24.2.7:
 *   wPortStatus  bit  0   current connect
 *                bit  1   port enabled
 *                bit  2   suspend
 *                bit  3   over-current
 *                bit  4   reset
 *                bit  8   power on
 *                bit  9   low-speed device attached
 */
#include "usb_hub.h"
#include "usb.h"
#include "usb_core.h"
#include "uhci.h"
#include "usb_hc.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"

/* Hub-class request constants. */
#define HUB_REQ_CLEAR_FEATURE   0x01
#define HUB_REQ_GET_STATUS      0x00
#define HUB_REQ_SET_FEATURE     0x03
#define HUB_REQ_GET_DESCRIPTOR  0x06

#define HUB_DT_HUB              0x29

/* Feature selectors. */
#define PORT_CONNECTION          0
#define PORT_ENABLE              1
#define PORT_RESET               4
#define PORT_POWER               8
#define C_PORT_CONNECTION       16
#define C_PORT_RESET            20

/* Port-status bits. */
#define PS_CONNECT          (1 <<  0)
#define PS_ENABLE           (1 <<  1)
#define PS_RESET            (1 <<  4)
#define PS_POWER            (1 <<  8)
#define PS_LOW_SPEED        (1 <<  9)

/* USB hub descriptor — only the first few fields we care about. */
struct hub_desc {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;       /* 0x29 */
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;        /* 2 ms units */
    uint8_t  bHubContrCurrent;
    /* Variable-length DeviceRemovable + PortPwrCtrlMask follow.
     * We don't read them for the demo. */
} __attribute__((packed));

static int hub_get_descriptor(struct usb_device *d, struct hub_desc *out) {
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
        .bRequest      = HUB_REQ_GET_DESCRIPTOR,
        .wValue        = (uint16_t)(HUB_DT_HUB << 8),
        .wIndex        = 0,
        .wLength       = sizeof(*out),
    };
    return d->hc->control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                 &s, out, sizeof(*out), /*data_in=*/1);
}

static int hub_set_port_feature(struct usb_device *d, int port, int feature) {
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
        .bRequest      = HUB_REQ_SET_FEATURE,
        .wValue        = (uint16_t)feature,
        .wIndex        = (uint16_t)port,
        .wLength       = 0,
    };
    return d->hc->control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                 &s, 0, 0, /*data_in=*/0);
}

static int hub_clear_port_feature(struct usb_device *d, int port, int feature) {
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
        .bRequest      = HUB_REQ_CLEAR_FEATURE,
        .wValue        = (uint16_t)feature,
        .wIndex        = (uint16_t)port,
        .wLength       = 0,
    };
    return d->hc->control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                 &s, 0, 0, /*data_in=*/0);
}

static int hub_get_port_status(struct usb_device *d, int port,
                               uint16_t *out_status, uint16_t *out_change) {
    uint8_t buf[4];
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
        .bRequest      = HUB_REQ_GET_STATUS,
        .wValue        = 0,
        .wIndex        = (uint16_t)port,
        .wLength       = 4,
    };
    int rc = d->hc->control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                   &s, buf, 4, /*data_in=*/1);
    if (rc != USB_OK) return rc;
    *out_status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    *out_change = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    return USB_OK;
}

/* Drive one port through power-on → reset → enable, then call
 * usb_enumerate_default if a device is attached. */
static void enumerate_port(struct usb_device *hub, int port,
                           int pwr_on_2ms_units)
{
    /* Power the port. */
    if (hub_set_port_feature(hub, port, PORT_POWER) != USB_OK) {
        kprintf("[hub] addr %d port %d: POWER feature failed\n",
                hub->addr, port);
        return;
    }
    /* bPwrOn2PwrGood is in 2 ms units. Floor at 20 ms because a bunch
     * of cheap hubs lie about this value. */
    int pwr_ms = pwr_on_2ms_units * 2;
    if (pwr_ms < 20) pwr_ms = 20;
    pit_sleep((uint32_t)pwr_ms);

    /* Check for a connected device. */
    uint16_t status = 0, change = 0;
    if (hub_get_port_status(hub, port, &status, &change) != USB_OK) {
        kprintf("[hub] addr %d port %d: GET_STATUS failed\n",
                hub->addr, port);
        return;
    }
    if (!(status & PS_CONNECT)) {
        return;     /* port empty, quiet */
    }
    /* Acknowledge connect-change so we don't trip over it later. */
    if (change & 1) hub_clear_port_feature(hub, port, C_PORT_CONNECTION);

    /* Reset the port. USB 1.1 §11.5.1.5 says hold ≥10 ms; 50 ms is
     * the standard rule-of-thumb. After the reset, the hub itself
     * brings PORT_ENABLE high and clears PORT_RESET. */
    if (hub_set_port_feature(hub, port, PORT_RESET) != USB_OK) {
        kprintf("[hub] addr %d port %d: SET_RESET failed\n",
                hub->addr, port);
        return;
    }
    pit_sleep(60);

    /* Poll until reset clears (or timeout). */
    int reset_cleared = 0;
    for (int t = 0; t < 16; t++) {
        if (hub_get_port_status(hub, port, &status, &change) != USB_OK) {
            kprintf("[hub] addr %d port %d: poll GET_STATUS failed\n",
                    hub->addr, port);
            return;
        }
        if (!(status & PS_RESET)) { reset_cleared = 1; break; }
        pit_sleep(10);
    }
    if (!reset_cleared) {
        kprintf("[hub] addr %d port %d: reset never cleared (status=%x)\n",
                hub->addr, port, (unsigned)status);
        return;
    }
    /* Clear the reset-change indication. */
    hub_clear_port_feature(hub, port, C_PORT_RESET);

    if (!(status & PS_ENABLE)) {
        kprintf("[hub] addr %d port %d: not enabled after reset (status=%x)\n",
                hub->addr, port, (unsigned)status);
        return;
    }

    int low_speed = (status & PS_LOW_SPEED) ? 1 : 0;
    kprintf("[hub] addr %d port %d: %s-speed device, enumerating\n",
            hub->addr, port, low_speed ? "low" : "full");

    /* Hand to the core enumerator. It does the standard
     * GET_DESCRIPTOR / SET_ADDRESS / config dance against the device
     * now sitting at default address behind this hub port. */
    char tag[24];
    int o = 0;
    tag[o++] = 'h'; tag[o++] = 'u'; tag[o++] = 'b'; tag[o++] = ' ';
    if (hub->addr >= 10) tag[o++] = (char)('0' + hub->addr / 10);
    tag[o++] = (char)('0' + hub->addr % 10);
    tag[o++] = ' '; tag[o++] = 'p'; tag[o++] = 'o'; tag[o++] = 'r'; tag[o++] = 't';
    tag[o++] = ' ';
    if (port >= 10) tag[o++] = (char)('0' + port / 10);
    tag[o++] = (char)('0' + port % 10);
    tag[o] = 0;
    /* Inherit the hub's host controller — devices behind a hub
     * always live on the same HC as the hub itself. */
    usb_enumerate_default(low_speed, tag, hub->hc);
}

void usb_hub_attach(struct usb_device *d) {
    struct hub_desc desc;
    memset(&desc, 0, sizeof(desc));
    if (hub_get_descriptor(d, &desc) != USB_OK) {
        kprintf("[hub] addr %d: GET_DESCRIPTOR(HUB) failed\n", d->addr);
        return;
    }
    int n_ports = desc.bNbrPorts;
    if (n_ports < 1 || n_ports > 16) {
        kprintf("[hub] addr %d: bogus bNbrPorts=%d\n", d->addr, n_ports);
        return;
    }
    kprintf("[hub] addr %d: %d ports, pwr2good=%dms\n",
            d->addr, n_ports, desc.bPwrOn2PwrGood * 2);

    for (int p = 1; p <= n_ports; p++) {
        enumerate_port(d, p, desc.bPwrOn2PwrGood);
    }
}
