/*
 * USB CDC-ACM driver — "USB serial port".
 *
 * CDC-ACM devices present two interfaces:
 *
 *   Comm interface (class 0x02, subclass 0x02 ACM):
 *     - One interrupt-IN endpoint for status notifications
 *       (we ignore notifications — they're optional for our use)
 *     - Class requests: SET_LINE_CODING, SET_CONTROL_LINE_STATE
 *
 *   Data interface (class 0x0A):
 *     - One bulk-IN endpoint   (device -> host)
 *     - One bulk-OUT endpoint  (host   -> device)
 *
 * Our driver only uses the bulk pipes. We do send one
 * SET_CONTROL_LINE_STATE(DTR=1|RTS=1) at attach time — most CDC-ACM
 * firmwares gate TX on DTR, so without this nothing comes out.
 *
 * Polling task: every 50 ms try a non-blocking bulk-IN read. NAKs are
 * common (no data ready) and silently ignored. Received bytes are
 * printed via kprintf so they show up on the kernel serial console.
 *
 * Write path: usb_cdc_acm_write(data, len) is callable from any
 * kernel context that holds the BKL.
 */
#include "usb_cdc_acm.h"
#include "usb.h"
#include "usb_core.h"
#include "uhci.h"
#include "kprintf.h"
#include "string.h"
#include "kmalloc.h"
#include "pit.h"
#include "task.h"
#include "spinlock.h"

#define MAX_CDC_DEVICES  2

struct cdc_device {
    struct usb_device *dev;
    int                comm_iface;
    int                data_iface;
    int                ep_in;
    int                ep_out;
    int                ep_max;
    int                in_toggle;
    int                out_toggle;
    int                in_use;
    spinlock_t         tx_lock;
};

static struct cdc_device g_cdc[MAX_CDC_DEVICES];
static int               g_n_cdc;

/* ---- class request: SET_CONTROL_LINE_STATE ---------------------- */

static int cdc_set_control_line_state(struct cdc_device *c,
                                      int dtr, int rts)
{
    /* bRequestType: OUT, CLASS, INTERFACE (comm interface).
     * wValue: bit 0 = DTR, bit 1 = RTS. */
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        .bRequest      = USB_CDC_REQ_SET_CONTROL_LINE_STATE,
        .wValue        = (uint16_t)((dtr ? 1 : 0) | (rts ? 2 : 0)),
        .wIndex        = (uint16_t)c->comm_iface,
        .wLength       = 0,
    };
    return uhci_control_transfer(c->dev->addr, c->dev->low_speed,
                                 c->dev->ep0_max_packet,
                                 &s, 0, 0, 0);
}

static int cdc_set_line_coding(struct cdc_device *c,
                               uint32_t baud, uint8_t stop,
                               uint8_t parity, uint8_t bits)
{
    /* 7-byte line coding struct (USB CDC §6.2.13):
     *   [0..3] dwDTERate (baud, little-endian)
     *   [4]    bCharFormat   (0 = 1 stop, 1 = 1.5, 2 = 2)
     *   [5]    bParityType   (0=N, 1=O, 2=E, 3=M, 4=S)
     *   [6]    bDataBits     (5/6/7/8/16) */
    uint8_t lc[7];
    lc[0] = (uint8_t)(baud      );
    lc[1] = (uint8_t)(baud >>  8);
    lc[2] = (uint8_t)(baud >> 16);
    lc[3] = (uint8_t)(baud >> 24);
    lc[4] = stop;
    lc[5] = parity;
    lc[6] = bits;
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        .bRequest      = USB_CDC_REQ_SET_LINE_CODING,
        .wValue        = 0,
        .wIndex        = (uint16_t)c->comm_iface,
        .wLength       = 7,
    };
    return uhci_control_transfer(c->dev->addr, c->dev->low_speed,
                                 c->dev->ep0_max_packet,
                                 &s, lc, 7, 0);
}

/* ---- attach ----------------------------------------------------- */

void usb_cdc_acm_attach(struct usb_device *d, int data_iface,
                        int comm_iface, int ep_in, int ep_out, int ep_max)
{
    if (g_n_cdc >= MAX_CDC_DEVICES) {
        kprintf("[cdc-acm] no free slot\n");
        return;
    }
    if (ep_max <= 0) ep_max = 64;

    struct cdc_device *c = &g_cdc[g_n_cdc];
    memset(c, 0, sizeof(*c));
    c->dev        = d;
    c->comm_iface = comm_iface;
    c->data_iface = data_iface;
    c->ep_in      = ep_in;
    c->ep_out     = ep_out;
    c->ep_max     = ep_max;
    c->in_toggle  = 0;
    c->out_toggle = 0;
    c->in_use     = 1;
    spin_lock_init(&c->tx_lock);

    /* Configure the line: 115200 8N1. Most CDC-ACM devices accept
     * anything (the rate is informational over USB), but some firmware
     * gates TX until line coding is set. */
    int rc = cdc_set_line_coding(c, 115200, 0, 0, 8);
    if (rc != USB_OK) {
        /* Non-fatal — many emulated devices respond STALL to this and
         * still work for raw I/O. */
        kprintf("[cdc-acm] SET_LINE_CODING rc=%d (continuing)\n", rc);
    }

    /* Assert DTR + RTS — without this most USB-serial firmware
     * silently drops TX. */
    rc = cdc_set_control_line_state(c, 1, 1);
    if (rc != USB_OK) {
        kprintf("[cdc-acm] SET_CONTROL_LINE_STATE rc=%d (continuing)\n", rc);
    }

    g_n_cdc++;
    kprintf("[cdc-acm] addr %d  data_iface=%d  ep_in=IN%d  ep_out=OUT%d  max=%d\n",
            d->addr, data_iface, ep_in, ep_out, ep_max);
}

/* ---- write path ------------------------------------------------- */

int usb_cdc_acm_write(const void *data, int len) {
    if (g_n_cdc == 0) return -1;
    struct cdc_device *c = &g_cdc[0];     /* first device wins */
    if (!c->in_use) return -1;
    if (len <= 0 || len > 4096) return -1;

    /* uhci_bulk_out wants a physically-contiguous source. Caller's
     * buffer is whatever they had — copy into kmalloc-backed storage
     * to satisfy that. */
    void *kbuf = kmalloc(len);
    if (!kbuf) return -1;
    memcpy(kbuf, data, len);

    spin_lock(&c->tx_lock);
    int rc = uhci_bulk_out(c->dev->addr, c->ep_max, c->ep_out,
                           kbuf, len, &c->out_toggle);
    spin_unlock(&c->tx_lock);

    kfree(kbuf);
    return rc >= 0 ? rc : -1;
}

/* ---- RX polling ------------------------------------------------- */

static void cdc_poll_one(struct cdc_device *c) {
    /* Read up to ep_max bytes. NAK is normal (no data). */
    uint8_t buf[64];
    int max = c->ep_max < 64 ? c->ep_max : 64;
    int rc = uhci_bulk_in(c->dev->addr, c->ep_max, c->ep_in,
                          buf, max, &c->in_toggle);
    if (rc == USB_ERR_NAK || rc == USB_ERR_TIMEOUT) return;
    if (rc < 0) {
        /* Persistent error — back off but don't disable. */
        return;
    }
    /* Route incoming bytes to the kernel serial sink. */
    for (int i = 0; i < rc; i++) {
        kputc((char)buf[i]);
    }
}

static void usb_cdc_acm_rx_task(void) {
    for (;;) {
        for (int i = 0; i < g_n_cdc; i++) {
            if (g_cdc[i].in_use) cdc_poll_one(&g_cdc[i]);
        }
        /* 50 ms — comparable to terminal echo latency, low CPU drain. */
        pit_sleep(50);
    }
}

void usb_cdc_acm_start_polling(void) {
    if (g_n_cdc == 0) return;
    task_make_runnable(task_create(usb_cdc_acm_rx_task, "usb-cdc-acm"));
    kprintf("[cdc-acm] RX polling task started\n");
}
