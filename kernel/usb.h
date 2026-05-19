/*
 * AdventOS USB common types — what every USB driver and class
 * speaks. Mostly RFC-style structs that go on the wire as
 * little-endian byte streams during enumeration.
 *
 * Sources:
 *   - USB 1.1 spec chapter 9 (Device Framework)
 *   - USB HID 1.11 (boot keyboard report format)
 *   - UHCI 1.1 (the host controller spec)
 *
 * Scope of this header: data structures and constants. Functions
 * live in usb_core.h, uhci.h, usb_hid.h.
 */
#ifndef ADVENTOS_USB_H
#define ADVENTOS_USB_H

#include "../include/types.h"

/* ---- Standard request types (bmRequestType) -------------------- */
#define USB_DIR_OUT             0x00
#define USB_DIR_IN              0x80

#define USB_TYPE_STANDARD       (0 << 5)
#define USB_TYPE_CLASS          (1 << 5)
#define USB_TYPE_VENDOR         (2 << 5)

#define USB_RECIP_DEVICE        0
#define USB_RECIP_INTERFACE     1
#define USB_RECIP_ENDPOINT      2
#define USB_RECIP_OTHER         3      /* used by hub class for per-port requests */

/* ---- Standard request codes (bRequest) ------------------------- */
#define USB_REQ_GET_STATUS         0
#define USB_REQ_CLEAR_FEATURE      1
#define USB_REQ_SET_FEATURE        3
#define USB_REQ_SET_ADDRESS        5
#define USB_REQ_GET_DESCRIPTOR     6
#define USB_REQ_SET_DESCRIPTOR     7
#define USB_REQ_GET_CONFIGURATION  8
#define USB_REQ_SET_CONFIGURATION  9
#define USB_REQ_GET_INTERFACE     10
#define USB_REQ_SET_INTERFACE     11

/* HID class requests (sent with USB_TYPE_CLASS | USB_RECIP_INTERFACE) */
#define USB_HID_REQ_GET_REPORT     0x01
#define USB_HID_REQ_GET_IDLE       0x02
#define USB_HID_REQ_GET_PROTOCOL   0x03
#define USB_HID_REQ_SET_REPORT     0x09
#define USB_HID_REQ_SET_IDLE       0x0A
#define USB_HID_REQ_SET_PROTOCOL   0x0B

#define USB_HID_PROTOCOL_BOOT      0
#define USB_HID_PROTOCOL_REPORT    1

/* ---- Descriptor types ------------------------------------------ */
#define USB_DT_DEVICE              1
#define USB_DT_CONFIG              2
#define USB_DT_STRING              3
#define USB_DT_INTERFACE           4
#define USB_DT_ENDPOINT            5
#define USB_DT_HID                 0x21
#define USB_DT_REPORT              0x22

/* ---- Class codes ----------------------------------------------- */
#define USB_CLASS_CDC_COMM         0x02   /* CDC Communication interface */
#define USB_CLASS_HID              0x03
#define USB_CLASS_MASS_STORAGE     0x08
#define USB_CLASS_HUB              0x09
#define USB_CLASS_CDC_DATA         0x0A   /* CDC Data interface (bulk in/out) */

/* CDC subclass codes */
#define USB_CDC_SUBCLASS_ACM       0x02   /* Abstract Control Model */
#define USB_CDC_SUBCLASS_ECM       0x06   /* Ethernet Networking Control Model */

/* CDC-ACM class requests */
#define USB_CDC_REQ_SET_LINE_CODING         0x20
#define USB_CDC_REQ_GET_LINE_CODING         0x21
#define USB_CDC_REQ_SET_CONTROL_LINE_STATE  0x22

/* CDC-ECM class requests */
#define USB_CDC_REQ_SET_ETHERNET_PACKET_FILTER  0x43

/* CDC functional descriptor subtypes (appear inside the
 * configuration blob, btype = 0x24, blen + bsubtype + ...). */
#define USB_CDC_FUNC_HEADER         0x00
#define USB_CDC_FUNC_UNION          0x06
#define USB_CDC_FUNC_ETHERNET       0x0F  /* Ethernet networking functional desc */

/* HID subclass / protocol (boot interface) */
#define USB_HID_SUBCLASS_BOOT      0x01
#define USB_HID_PROTOCOL_KEYBOARD  0x01
#define USB_HID_PROTOCOL_MOUSE     0x02

/* ---- Endpoint attributes --------------------------------------- */
#define USB_EP_DIR_MASK            0x80
#define USB_EP_NUM_MASK            0x0F
#define USB_EP_TYPE_MASK           0x03
#define USB_EP_TYPE_CONTROL        0
#define USB_EP_TYPE_ISOCHRONOUS    1
#define USB_EP_TYPE_BULK           2
#define USB_EP_TYPE_INTERRUPT      3

/* ---- Setup packet (8 bytes, sent in the SETUP stage) ----------- */
struct usb_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

/* ---- Standard descriptors (all little-endian on the wire) ------ */

struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;       /* = USB_DT_DEVICE */
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;       /* MaxPacketSize for endpoint 0 */
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;       /* = USB_DT_CONFIG */
    uint16_t wTotalLength;          /* config + iface(s) + ep(s) + class */
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;             /* in 2 mA units */
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;       /* = USB_DT_INTERFACE */
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;       /* = USB_DT_ENDPOINT */
    uint8_t  bEndpointAddress;      /* high bit = direction (1 = IN) */
    uint8_t  bmAttributes;          /* low 2 bits = transfer type */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;             /* in frames (1 ms each, low/full speed) */
} __attribute__((packed));

/* ---- Transfer status codes returned by host-controller drivers - */
#define USB_OK              0
#define USB_ERR_TIMEOUT    -1
#define USB_ERR_STALL      -2
#define USB_ERR_BABBLE     -3
#define USB_ERR_CRC        -4
#define USB_ERR_NAK        -5
#define USB_ERR_OTHER      -6

struct usb_hc_ops;          /* defined in usb_hc.h */

/* Per-device record. The host controller driver populates `addr`,
 * `low_speed`, and `hc` after enumeration; class drivers fill in
 * the endpoint fields they care about. */
struct usb_device {
    uint8_t  addr;                  /* assigned address (1..127) */
    uint8_t  low_speed;             /* 1 if attached as 1.5 Mbps */
    uint8_t  ep0_max_packet;        /* 8/16/32/64 */
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  in_use;
    const struct usb_hc_ops *hc;    /* which host controller drives this
                                     * device (uhci / ehci). Set during
                                     * enumeration; used by every
                                     * transfer call. */
};

#endif
