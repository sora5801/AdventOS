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

/* Forward decls from class drivers — keep them here to avoid a
 * separate header per class. */
void usb_hid_attach(struct usb_device *d,
                    const uint8_t *cfg_buf, int cfg_len,
                    int iface_num, int proto, int ep_addr,
                    int ep_max, int ep_interval);
void usb_msc_attach(struct usb_device *d,
                    int iface_num, int ep_in, int ep_out, int ep_max);
void usb_hub_attach(struct usb_device *d);
void usb_cdc_acm_attach(struct usb_device *d, int data_iface,
                        int comm_iface, int ep_in, int ep_out, int ep_max);

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

/* Find a CDC-ACM data interface (class 0x0A) plus its bulk-IN/OUT
 * endpoints. CDC-ACM devices have two interfaces; the comm interface
 * (class 0x02, subclass 0x02 ACM) carries the SET_LINE_CODING /
 * SET_CONTROL_LINE_STATE class requests; the data interface holds
 * the actual byte pipes.
 *
 * Returns 1 on success and fills the out-params; 0 otherwise. */
static int find_cdc_acm_interface(const uint8_t *cfg, int cfg_len,
                                  int *comm_iface, int *data_iface,
                                  int *ep_in, int *ep_out, int *ep_max)
{
    int o = 0;
    int saw_comm = 0;
    int cur_comm = -1;
    int cur_data = -1;
    int in_data  = 0;
    int found_in = 0, found_out = 0, max_in = 0, max_out = 0;

    while (o + 2 <= cfg_len) {
        uint8_t blen  = cfg[o];
        uint8_t btype = cfg[o + 1];
        if (blen == 0 || o + blen > cfg_len) return 0;

        if (btype == USB_DT_INTERFACE && blen >= 9) {
            const struct usb_interface_descriptor *id =
                (const struct usb_interface_descriptor *)(cfg + o);
            if (id->bInterfaceClass == USB_CLASS_CDC_COMM &&
                id->bInterfaceSubClass == USB_CDC_SUBCLASS_ACM)
            {
                saw_comm = 1;
                cur_comm = id->bInterfaceNumber;
                in_data  = 0;
            } else if (id->bInterfaceClass == USB_CLASS_CDC_DATA) {
                in_data  = 1;
                cur_data = id->bInterfaceNumber;
                found_in = found_out = 0;
                max_in   = max_out   = 0;
            } else {
                in_data = 0;
            }
        } else if (btype == USB_DT_ENDPOINT && blen >= 7 && in_data) {
            const struct usb_endpoint_descriptor *ed =
                (const struct usb_endpoint_descriptor *)(cfg + o);
            if ((ed->bmAttributes & USB_EP_TYPE_MASK) == USB_EP_TYPE_BULK) {
                if (ed->bEndpointAddress & USB_EP_DIR_MASK) {
                    found_in = ed->bEndpointAddress & USB_EP_NUM_MASK;
                    max_in   = ed->wMaxPacketSize;
                } else {
                    found_out = ed->bEndpointAddress & USB_EP_NUM_MASK;
                    max_out   = ed->wMaxPacketSize;
                }
            }
        }
        o += blen;
    }

    if (saw_comm && found_in && found_out) {
        *comm_iface = cur_comm;
        *data_iface = cur_data;
        *ep_in      = found_in;
        *ep_out     = found_out;
        *ep_max     = max_in < max_out ? max_in : max_out;
        if (*ep_max == 0) *ep_max = 64;
        return 1;
    }
    return 0;
}

/* Same idea for USB Mass Storage Class — we need an interface with
 * bInterfaceClass=0x08 plus a bulk-IN and bulk-OUT endpoint pair.
 * Returns 1 on success and fills iface_num + ep_in + ep_out + ep_max
 * (the latter being min(in_max, out_max), which is always 64 for
 * full-speed BOT in practice). */
static int find_msc_interface(const uint8_t *cfg, int cfg_len,
                              int *iface_num,
                              int *ep_in, int *ep_out, int *ep_max)
{
    int o = 0;
    int in_msc = 0;
    int cur_iface = -1;
    int found_in = 0, found_out = 0;
    int max_in = 0, max_out = 0;

    while (o + 2 <= cfg_len) {
        uint8_t blen  = cfg[o];
        uint8_t btype = cfg[o + 1];
        if (blen == 0 || o + blen > cfg_len) return 0;

        if (btype == USB_DT_INTERFACE && blen >= 9) {
            /* Crossing into a new interface — if we'd accumulated
             * a complete IN+OUT pair on the previous one, return now. */
            if (in_msc && found_in && found_out) {
                *iface_num = cur_iface;
                *ep_in     = found_in;
                *ep_out    = found_out;
                *ep_max    = max_in < max_out ? max_in : max_out;
                if (*ep_max == 0) *ep_max = 64;
                return 1;
            }
            const struct usb_interface_descriptor *id =
                (const struct usb_interface_descriptor *)(cfg + o);
            in_msc = (id->bInterfaceClass == USB_CLASS_MASS_STORAGE);
            cur_iface = id->bInterfaceNumber;
            found_in = found_out = 0;
            max_in = max_out = 0;
        } else if (btype == USB_DT_ENDPOINT && blen >= 7 && in_msc) {
            const struct usb_endpoint_descriptor *ed =
                (const struct usb_endpoint_descriptor *)(cfg + o);
            if ((ed->bmAttributes & USB_EP_TYPE_MASK) == USB_EP_TYPE_BULK) {
                if (ed->bEndpointAddress & USB_EP_DIR_MASK) {
                    found_in  = ed->bEndpointAddress & USB_EP_NUM_MASK;
                    max_in    = ed->wMaxPacketSize;
                } else {
                    found_out = ed->bEndpointAddress & USB_EP_NUM_MASK;
                    max_out   = ed->wMaxPacketSize;
                }
            }
        }
        o += blen;
    }
    /* Tail case: last interface in the blob was MSC and complete. */
    if (in_msc && found_in && found_out) {
        *iface_num = cur_iface;
        *ep_in     = found_in;
        *ep_out    = found_out;
        *ep_max    = max_in < max_out ? max_in : max_out;
        if (*ep_max == 0) *ep_max = 64;
        return 1;
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

void usb_enumerate_default(int low_speed, const char *origin) {
    struct usb_device *d = alloc_device(low_speed);
    if (!d) {
        kprintf("[usb] no free device slot for %s\n", origin);
        return;
    }

    /* Step 2: pull the first 8 bytes of the device descriptor at
     * the default address. Tells us the real ep0 max packet size. */
    uint8_t dd_buf[18];
    int rc = usb_get_descriptor(d, USB_DT_DEVICE, 0, 0, dd_buf, 8);
    if (rc != USB_OK) {
        kprintf("[usb] %s: GET_DESCRIPTOR(8B) failed rc=%d\n",
                origin, rc);
        d->in_use = 0;
        return;
    }
    d->ep0_max_packet = dd_buf[7];
    if (d->ep0_max_packet == 0) d->ep0_max_packet = 8;

    /* Step 3: SET_ADDRESS. */
    uint8_t new_addr = g_next_addr++;
    rc = usb_set_address(d, new_addr);
    if (rc != USB_OK) {
        kprintf("[usb] %s: SET_ADDRESS(%d) failed rc=%d\n",
                origin, new_addr, rc);
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

    /* Step 8: bind a class driver.
     *
     * Hub class (0x09) is checked first via the device's bDeviceClass
     * field (it's a device-level class, not an interface-level one
     * like HID/MSC). For class-9 devices we don't bother parsing the
     * config for interfaces — the hub class binds to the whole device. */
    if (dd->bDeviceClass == USB_CLASS_HUB) {
        kprintf("[usb] addr %d: USB hub\n", d->addr);
        usb_hub_attach(d);
        kfree(full_cfg);
        return;
    }

    int iface, proto, ep, ep_max, ep_int;
    int ep_in, ep_out;
    int comm_iface, data_iface;

    if (find_hid_interface(full_cfg, total_len,
                           &iface, &proto, &ep, &ep_max, &ep_int)) {
        kprintf("[usb] addr %d: HID iface=%d proto=%d ep=IN%d max=%d int=%dms\n",
                d->addr, iface, proto, ep, ep_max, ep_int);
        usb_hid_attach(d, full_cfg, total_len,
                       iface, proto, ep, ep_max, ep_int);
    } else if (find_msc_interface(full_cfg, total_len,
                                  &iface, &ep_in, &ep_out, &ep_max)) {
        kprintf("[usb] addr %d: MSC iface=%d  ep_in=IN%d  ep_out=OUT%d  max=%d\n",
                d->addr, iface, ep_in, ep_out, ep_max);
        usb_msc_attach(d, iface, ep_in, ep_out, ep_max);
        kfree(full_cfg);
    } else if (find_cdc_acm_interface(full_cfg, total_len,
                                      &comm_iface, &data_iface,
                                      &ep_in, &ep_out, &ep_max)) {
        kprintf("[usb] addr %d: CDC-ACM comm=%d data=%d "
                "ep_in=IN%d ep_out=OUT%d max=%d\n",
                d->addr, comm_iface, data_iface,
                ep_in, ep_out, ep_max);
        usb_cdc_acm_attach(d, data_iface, comm_iface,
                           ep_in, ep_out, ep_max);
        kfree(full_cfg);
    } else {
        kprintf("[usb] addr %d: no recognized class — leaving idle\n", d->addr);
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
            char tag[16];
            tag[0] = 'p'; tag[1] = 'o'; tag[2] = 'r'; tag[3] = 't';
            tag[4] = ' '; tag[5] = (char)('0' + i + 1); tag[6] = 0;
            usb_enumerate_default(low_speed[i], tag);
        } else {
            kprintf("[usb] port %d: no device\n", i + 1);
        }
    }
}
