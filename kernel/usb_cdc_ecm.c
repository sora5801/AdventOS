/*
 * USB CDC-ECM driver. See usb_cdc_ecm.h.
 *
 * Lifecycle:
 *   1. usb_core enumerates the device, finds Comm interface (class 0x02
 *      / subclass 0x06) + Data interface (class 0x0A) with two
 *      alternate settings (alt 0 = no endpoints, alt 1 = bulk-IN +
 *      bulk-OUT). Calls usb_cdc_ecm_attach.
 *   2. usb_cdc_ecm_attach issues SET_INTERFACE(data_iface, alt=1) to
 *      activate the bulk pipes, reads the iMACAddress string
 *      descriptor for our MAC, stores the device.
 *   3. net_init asks usb_cdc_ecm_init(&mac) — if we got an attach,
 *      it returns 0 and net.c installs usb_cdc_ecm_send as g_nic_send.
 *   4. usb_cdc_ecm_start_polling spawns a task that polls bulk-IN
 *      every 5 ms and routes each frame to net_rx_frame.
 *
 * Why polled instead of IRQ-driven: UHCI doesn't deliver async
 * completion IRQs in any usable way — the controller's IRQ fires
 * on schedule-list anomalies, not on individual TD completion.
 * Every other UHCI client (HID, MSC, CDC-ACM) polls; we match.
 * EHCI would be different but EHCI is the next driver in this push.
 */
#include "usb_cdc_ecm.h"
#include "usb.h"
#include "usb_core.h"
#include "uhci.h"
#include "net.h"
#include "kprintf.h"
#include "string.h"
#include "kmalloc.h"
#include "pit.h"
#include "task.h"
#include "spinlock.h"

#define ECM_MAX_FRAME    1518   /* standard Ethernet MTU + header */

struct ecm_device {
    struct usb_device *dev;
    int                comm_iface;
    int                data_iface;
    int                data_iface_alt1;
    int                ep_in;
    int                ep_out;
    int                ep_max;
    int                in_toggle;
    int                out_toggle;
    int                in_use;
    struct mac_addr    mac;
    spinlock_t         tx_lock;
};

static struct ecm_device g_ecm;

/* ---- helpers ---------------------------------------------------- */

/* SET_INTERFACE(iface, alt) — standard request §9.4.10. */
static int usb_set_interface(struct usb_device *d, int iface, int alt) {
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
        .bRequest      = USB_REQ_SET_INTERFACE,
        .wValue        = (uint16_t)alt,
        .wIndex        = (uint16_t)iface,
        .wLength       = 0,
    };
    return uhci_control_transfer(d->addr, d->low_speed, d->ep0_max_packet,
                                 &s, 0, 0, 0);
}

/* Parse 12 hex digits at `in` (UTF-16LE, so 24 bytes) into a 6-byte
 * MAC. Returns 1 on success, 0 on bad input. The Ethernet Functional
 * Descriptor §5.4 mandates exactly 12 uppercase ASCII hex digits. */
static int parse_mac_utf16(const uint8_t *in, int in_len,
                           struct mac_addr *out)
{
    if (in_len < 24) return 0;
    for (int i = 0; i < 6; i++) {
        uint8_t hi = in[i * 4 + 0];
        uint8_t lo = in[i * 4 + 2];
        int hv, lv;
        if      (hi >= '0' && hi <= '9') hv = hi - '0';
        else if (hi >= 'A' && hi <= 'F') hv = hi - 'A' + 10;
        else if (hi >= 'a' && hi <= 'f') hv = hi - 'a' + 10;
        else                              return 0;
        if      (lo >= '0' && lo <= '9') lv = lo - '0';
        else if (lo >= 'A' && lo <= 'F') lv = lo - 'A' + 10;
        else if (lo >= 'a' && lo <= 'f') lv = lo - 'a' + 10;
        else                              return 0;
        out->b[i] = (uint8_t)((hv << 4) | lv);
    }
    return 1;
}

/* Fetch string descriptor `idx` and parse it as a CDC-ECM MAC.
 * Falls back to a hardcoded MAC if anything goes wrong — better
 * to keep the link usable than to refuse to attach. */
static void fetch_mac(struct ecm_device *e, int imac_str_idx) {
    /* Default: locally-administered, prefix matches QEMU's defaults
     * so dhcp logs look familiar. Used only if the string fetch fails. */
    static const struct mac_addr fallback = {
        { 0x52, 0x54, 0x00, 0xCD, 0xC0, 0x01 }
    };
    if (imac_str_idx == 0) { e->mac = fallback; return; }

    uint8_t buf[64];
    int rc = usb_get_descriptor(e->dev, USB_DT_STRING,
                                (uint8_t)imac_str_idx, 0x0409,
                                buf, sizeof(buf));
    if (rc != USB_OK || buf[0] < 2 || buf[1] != USB_DT_STRING) {
        e->mac = fallback;
        return;
    }
    int payload = buf[0] - 2;
    if (!parse_mac_utf16(buf + 2, payload, &e->mac)) {
        e->mac = fallback;
    }
}

/* ---- attach ----------------------------------------------------- */

void usb_cdc_ecm_attach(struct usb_device *d,
                        int comm_iface, int data_iface,
                        int data_iface_alt1,
                        int ep_in, int ep_out, int ep_max,
                        int imac_str_idx)
{
    if (g_ecm.in_use) {
        kprintf("[cdc-ecm] another ECM device already attached — ignoring\n");
        return;
    }
    if (ep_max <= 0) ep_max = 64;

    struct ecm_device *e = &g_ecm;
    memset(e, 0, sizeof(*e));
    e->dev             = d;
    e->comm_iface      = comm_iface;
    e->data_iface      = data_iface;
    e->data_iface_alt1 = data_iface_alt1;
    e->ep_in           = ep_in;
    e->ep_out          = ep_out;
    e->ep_max          = ep_max;
    spin_lock_init(&e->tx_lock);

    /* Activate the data interface — alt 0 has no endpoints, alt 1
     * exposes the bulk pair. Without this SET_INTERFACE the controller
     * NAKs every bulk transfer. */
    int rc = usb_set_interface(d, data_iface, data_iface_alt1);
    if (rc != USB_OK) {
        kprintf("[cdc-ecm] SET_INTERFACE(%d, alt=%d) rc=%d — aborting\n",
                data_iface, data_iface_alt1, rc);
        return;
    }

    fetch_mac(e, imac_str_idx);
    e->in_use = 1;

    kprintf("[cdc-ecm] addr %d  iface comm=%d data=%d(alt%d)  "
            "ep_in=IN%d ep_out=OUT%d max=%d\n",
            d->addr, comm_iface, data_iface, data_iface_alt1,
            ep_in, ep_out, ep_max);
    kprintf("[cdc-ecm] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            e->mac.b[0], e->mac.b[1], e->mac.b[2],
            e->mac.b[3], e->mac.b[4], e->mac.b[5]);
}

/* ---- NIC adapter -------------------------------------------- */

/* Called from net_init at boot — returns 0 if attach already happened,
 * fills MAC. -1 means "no CDC-ECM device, try the next NIC." */
int usb_cdc_ecm_init(struct mac_addr *out_mac) {
    if (!g_ecm.in_use) return -1;
    if (out_mac) *out_mac = g_ecm.mac;
    return 0;
}

int usb_cdc_ecm_send(const void *frame, uint32_t len) {
    struct ecm_device *e = &g_ecm;
    if (!e->in_use)                        return -1;
    if (len == 0 || len > ECM_MAX_FRAME)   return -1;

    /* uhci_bulk_out wants a contiguous physically-addressable buffer;
     * the caller's frame might be on a transient stack page. Copy. */
    void *kbuf = kmalloc((int)len);
    if (!kbuf) return -1;
    memcpy(kbuf, frame, len);

    spin_lock(&e->tx_lock);
    int rc = uhci_bulk_out(e->dev->addr, e->ep_max, e->ep_out,
                           kbuf, (int)len, &e->out_toggle);
    spin_unlock(&e->tx_lock);

    kfree(kbuf);
    return rc >= 0 ? (int)len : -1;
}

/* ---- RX polling -------------------------------------------- */

static uint8_t g_rx_buf[ECM_MAX_FRAME];

static void ecm_poll_one(struct ecm_device *e) {
    /* One bulk-IN per frame. CDC-ECM §3.3.1 mandates one Ethernet
     * frame per transfer; short packet (or ZLP) terminates. */
    int rc = uhci_bulk_in(e->dev->addr, e->ep_max, e->ep_in,
                          g_rx_buf, ECM_MAX_FRAME, &e->in_toggle);
    if (rc == USB_ERR_NAK || rc == USB_ERR_TIMEOUT) return;
    if (rc <= 0)  return;
    if (rc < 14) return;        /* shorter than Eth header — drop */
    net_rx_frame(g_rx_buf, (uint32_t)rc);
}

static void usb_cdc_ecm_rx_task(void) {
    for (;;) {
        if (g_ecm.in_use) ecm_poll_one(&g_ecm);
        /* 5 ms — match TCP retransmit granularity; gives a USB-tier
         * latency that's still cheap on CPU when the pipe is idle
         * (NAK costs one TD round-trip). */
        pit_sleep(5);
    }
}

void usb_cdc_ecm_start_polling(void) {
    if (!g_ecm.in_use) return;
    task_make_runnable(task_create(usb_cdc_ecm_rx_task, "usb-cdc-ecm"));
    kprintf("[cdc-ecm] RX polling task started\n");
}
