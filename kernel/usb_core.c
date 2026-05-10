/*
 * USB core — port-walks both UHCI root-hub ports, enumerates each
 * attached device through the standard USB 1.1 chapter-9 sequence,
 * and binds class drivers (this session: HID; next session: mass
 * storage).
 *
 * Enumeration sequence per USB 1.1 §9.1.2:
 *
 *   1. Reset port (host controller does this in uhci_probe_ports).
 *   2. With device at default address 0, GET_DESCRIPTOR(DEVICE) for
 *      8 bytes — just enough to learn bMaxPacketSize0. UHCI lets
 *      us request 64 and get back whatever the endpoint actually
 *      supports (8/16/32/64), but for low-speed devices ep0 is
 *      always 8, so we request 8 to keep things simple.
 *   3. SET_ADDRESS to a fresh address. Wait ≥2 ms (USB spec).
 *   4. GET_DESCRIPTOR(DEVICE) for the full 18 bytes.
 *   5. GET_DESCRIPTOR(CONFIGURATION) for 9 bytes — learn wTotalLength.
 *   6. GET_DESCRIPTOR(CONFIGURATION) for wTotalLength bytes — gets
 *      every interface + endpoint + class descriptor in one shot.
 *   7. SET_CONFIGURATION(1).
 *   8. Hand to class driver.
 *
 * Errors at any step abort that device; the others enumerate fine.
 */
#include "usb_core.h"
#include "uhci.h"
#include "usb.h"
#include "kprintf.h"
#include "string.h"
#include "kmalloc.h"
#include "pit.h"

/* Forward decl from usb_hid.c so we don't need a separate header. */
void usb_hid_attach(struct usb_device *d,
                    const uint8_t *cfg_buf, int cfg_len,
                    int iface_num, int proto, int ep_addr,
                    int ep_max, int ep_interval);

/* Up to 4 simultaneous USB devices total (2 ports × possible
 * future hubs). Plenty for the demo. */
#define USB_MAX_DEVICES   4
static struct usb_device g_devices[USB_MAX_DEVICES];
static uint8_t           g_next_addr = 1;

/* ---- Wrappers around uhci_control_transfer --------------------- */

int usb_get_descriptor(struct usb_device *d,
                       uint8_t type, uint8_t index, uint16_t lang,
                       void *out, int max_len)
{
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest      = USB_REQ_GET_DESCRIPTOR,
        .wValue        = (uint16_t)((type << 8) | index),
        .wIndex        = lang,
        .wLength       = (uint16_t)max_len,
    };
    return uhci_control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                 &s, out, max_len, /*data_in=*/1);
}

static int usb_set_address(struct usb_device *d, uint8_t new_addr) {
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest      = USB_REQ_SET_ADDRESS,
        .wValue        = new_addr,
        .wIndex        = 0,
        .wLength       = 0,
    };
    int rc = uhci_control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                   &s, 0, 0, /*data_in=*/0);
    if (rc == USB_OK) d->addr = new_addr;
    /* USB 1.1 §9.2.6.3: device has 2 ms to start using new address. */
    pit_sleep(2);
    return rc;
}

int usb_set_configuration(struct usb_device *d, uint8_t value) {
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest      = USB_REQ_SET_CONFIGURATION,
        .wValue        = value,
        .wIndex        = 0,
        .wLength       = 0,
    };
    return uhci_control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                 &s, 0, 0, /*data_in=*/0);
}

int usb_hid_set_protocol(struct usb_device *d, int interface, int protocol) {
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        .bRequest      = USB_HID_REQ_SET_PROTOCOL,
        .wValue        = (uint16_t)protocol,
        .wIndex        = (uint16_t)interface,
        .wLength       = 0,
    };
    return uhci_control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                 &s, 0, 0, /*data_in=*/0);
}

int usb_hid_set_idle(struct usb_device *d, int interface, int duration_4ms) {
    /* duration_4ms in units of 4 ms; 0 = infinite (only report on change),
     * which is what we want for boot keyboard polling. */
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        .bRequest      = USB_HID_REQ_SET_IDLE,
        .wValue        = (uint16_t)((duration_4ms & 0xFF) << 8),
        .wIndex        = (uint16_t)interface,
        .wLength       = 0,
    };
    return uhci_control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                 &s, 0, 0, /*data_in=*/0);
}

/* ---- Configuration descriptor walker --------------------------- */

/* Walk a fully-loaded configuration descriptor blob looking for a
 * HID interface (bInterfaceClass==0x03) and its first interrupt-IN
 * endpoint. On success, fills the out-params and returns 1. */
static int find_hid_interface(const uint8_t *cfg, int cfg_len,
                              int *iface_num, int *proto,
                              int *ep_addr, int *ep_max, int *ep_interval)
{
    int o = 0;
    int in_hid_iface = 0;
    int cur_iface = -1, cur_proto = 0;

    while (o + 2 <= cfg_len) {
        uint8_t blen  = cfg[o];
        uint8_t btype = cfg[o + 1];
        if (blen == 0 || o + blen > cfg_len) return 0;

        if (btype == USB_DT_INTERFACE && blen >= 9) {
            const struct usb_interface_descriptor *id =
                (const struct usb_interface_descriptor *)(cfg + o);
            if (id->bInterfaceClass == USB_CLASS_HID) {
                in_hid_iface = 1;
                cur_iface = id->bInterfaceNumber;
                cur_proto = id->bInterfaceProtocol;
            } else {
                in_hid_iface = 0;
            }
        } else if (btype == USB_DT_ENDPOINT && blen >= 7 && in_hid_iface) {
            const struct usb_endpoint_descriptor *ed =
                (const struct usb_endpoint_descriptor *)(cfg + o);
            if ((ed->bEndpointAddress & USB_EP_DIR_MASK) &&
                (ed->bmAttributes & USB_EP_TYPE_MASK) == USB_EP_TYPE_INTERRUPT)
            {
                *iface_num   = cur_iface;
                *proto       = cur_proto;
                *ep_addr     = ed->bEndpointAddress & USB_EP_NUM_MASK;
                *ep_max      = ed->wMaxPacketSize;
                *ep_interval = ed->bInterval;
                return 1;
            }
        }
        o += blen;
    }
    return 0;
}

/* ---- Per-device enumeration ------------------------------------ */

static struct usb_device *alloc_device(int low_speed) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_devices[i].in_use) {
            memset(&g_devices[i], 0, sizeof(g_devices[i]));
            g_devices[i].in_use        = 1;
            g_devices[i].addr          = 0;
            g_devices[i].low_speed     = low_speed;
            g_devices[i].ep0_max_packet = 8;     /* USB 1.1 default */
            return &g_devices[i];
        }
    }
    return 0;
}

static void enumerate_one(int port_idx, int low_speed) {
    struct usb_device *d = alloc_device(low_speed);
    if (!d) {
        kprintf("[usb] no free device slot for port %d\n", port_idx);
        return;
    }

    /* Step 2: pull the first 8 bytes of the device descriptor at
     * the default address. Tells us the real ep0 max packet size. */
    uint8_t dd_buf[18];
    int rc = usb_get_descriptor(d, USB_DT_DEVICE, 0, 0, dd_buf, 8);
    if (rc != USB_OK) {
        kprintf("[usb] port %d: GET_DESCRIPTOR(8B) failed rc=%d\n",
                port_idx, rc);
        d->in_use = 0;
        return;
    }
    d->ep0_max_packet = dd_buf[7];
    if (d->ep0_max_packet == 0) d->ep0_max_packet = 8;

    /* Step 3: SET_ADDRESS. */
    uint8_t new_addr = g_next_addr++;
    rc = usb_set_address(d, new_addr);
    if (rc != USB_OK) {
        kprintf("[usb] port %d: SET_ADDRESS(%d) failed rc=%d\n",
                port_idx, new_addr, rc);
        d->in_use = 0;
        return;
    }

    /* Step 4: full device descriptor. */
    rc = usb_get_descriptor(d, USB_DT_DEVICE, 0, 0, dd_buf, 18);
    if (rc != USB_OK) {
        kprintf("[usb] addr %d: full DEVICE descriptor failed rc=%d\n",
                d->addr, rc);
        d->in_use = 0;
        return;
    }
    const struct usb_device_descriptor *dd =
        (const struct usb_device_descriptor *)dd_buf;
    d->vendor_id  = dd->idVendor;
    d->product_id = dd->idProduct;
    kprintf("[usb] addr %d  %s-speed  vid=%x  pid=%x  class=%x  ep0_max=%d\n",
            d->addr, d->low_speed ? "low" : "full",
            d->vendor_id, d->product_id, dd->bDeviceClass, d->ep0_max_packet);

    /* Step 5+6: configuration descriptor. First read 9 bytes for
     * wTotalLength, then read the whole blob. */
    uint8_t cd_head[9];
    rc = usb_get_descriptor(d, USB_DT_CONFIG, 0, 0, cd_head, 9);
    if (rc != USB_OK) {
        kprintf("[usb] addr %d: CONFIG head failed rc=%d\n", d->addr, rc);
        d->in_use = 0;
        return;
    }
    const struct usb_config_descriptor *ch =
        (const struct usb_config_descriptor *)cd_head;
    int total_len = ch->wTotalLength;
    if (total_len < 9 || total_len > 256) {
        kprintf("[usb] addr %d: bogus wTotalLength=%d\n", d->addr, total_len);
        d->in_use = 0;
        return;
    }
    uint8_t *full_cfg = kmalloc(total_len);
    if (!full_cfg) { d->in_use = 0; return; }
    rc = usb_get_descriptor(d, USB_DT_CONFIG, 0, 0, full_cfg, total_len);
    if (rc != USB_OK) {
        kprintf("[usb] addr %d: full CONFIG failed rc=%d\n", d->addr, rc);
        kfree(full_cfg);
        d->in_use = 0;
        return;
    }

    /* Step 7: SET_CONFIGURATION. */
    rc = usb_set_configuration(d, ch->bConfigurationValue);
    if (rc != USB_OK) {
        kprintf("[usb] addr %d: SET_CONFIGURATION failed rc=%d\n", d->addr, rc);
        kfree(full_cfg);
        d->in_use = 0;
        return;
    }

    /* Step 8: bind a class driver. For now, only HID. */
    int iface, proto, ep, ep_max, ep_int;
    if (find_hid_interface(full_cfg, total_len,
                           &iface, &proto, &ep, &ep_max, &ep_int)) {
        kprintf("[usb] addr %d: HID iface=%d proto=%d ep=IN%d max=%d int=%dms\n",
                d->addr, iface, proto, ep, ep_max, ep_int);
        usb_hid_attach(d, full_cfg, total_len,
                       iface, proto, ep, ep_max, ep_int);
        /* HID driver keeps the cfg buffer if it needs to. We
         * intentionally don't free here. */
    } else {
        kprintf("[usb] addr %d: no HID interface — leaving idle\n", d->addr);
        kfree(full_cfg);
    }
}

/* ---- Top-level init -------------------------------------------- */

void usb_init(void) {
    if (uhci_init() != 0) {
        kprintf("[usb] no UHCI controller — USB stack disabled\n");
        return;
    }

    int connected[2], low_speed[2], n_ports = 2;
    uhci_probe_ports(connected, low_speed, &n_ports);

    for (int i = 0; i < n_ports; i++) {
        if (connected[i]) {
            kprintf("[usb] port %d: %s-speed device attached\n",
                    i + 1, low_speed[i] ? "low" : "full");
            enumerate_one(i, low_speed[i]);
        } else {
            kprintf("[usb] port %d: no device\n", i + 1);
        }
    }
}
