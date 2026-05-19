/*
 * USB Mass Storage Class driver.
 *
 * Speaks Bulk-Only Transport (BOT, USB MSC §5) carrying SCSI
 * Transparent Command Set (subclass 0x06) over the standard
 * full-speed bulk pipes. One CBW out, optional data phase,
 * one CSW in — that's the whole transport.
 *
 * BOT message layout (USB MSC §5.1, §5.2):
 *
 *   CBW (Command Block Wrapper, host → device, exactly 31 bytes)
 *     [0..3]   dCBWSignature      = 0x43425355 ("USBC", little-endian)
 *     [4..7]   dCBWTag            arbitrary; device echoes in CSW
 *     [8..11]  dCBWDataTransferLen total bytes in the data phase
 *     [12]     bmCBWFlags         bit 7 = direction (1=IN, 0=OUT)
 *     [13]     bCBWLUN            target LUN, 0..15
 *     [14]     bCBWCBLength       length of CBWCB (1..16)
 *     [15..30] CBWCB              the SCSI command itself
 *
 *   data phase (host ↔ device, dCBWDataTransferLen bytes)
 *
 *   CSW (Command Status Wrapper, device → host, exactly 13 bytes)
 *     [0..3]   dCSWSignature      = 0x53425355 ("USBS")
 *     [4..7]   dCSWTag            echoed dCBWTag
 *     [8..11]  dCSWDataResidue    bytes NOT transferred
 *     [12]     bCSWStatus         0=ok, 1=command failed, 2=phase
 *
 * SCSI commands implemented (just enough to drive a flash disk):
 *   0x00 TEST UNIT READY    — probe; no data
 *   0x12 INQUIRY            — vendor / product strings, 36 bytes
 *   0x25 READ CAPACITY (10) — last-LBA + block-size, 8 bytes
 *   0x28 READ (10)          — read N blocks
 *   0x2A WRITE (10)         — write N blocks
 *
 * The driver is single-LUN (LUN 0). A real USB hub with multiple
 * SCSI LUNs (rare) would need a GET_MAX_LUN class request and a
 * blkdev per LUN — out of scope.
 */
#include "usb_msc.h"
#include "usb.h"
#include "usb_core.h"
#include "uhci.h"
#include "usb_hc.h"
#include "blkdev.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"

/* ---- Per-device state ------------------------------------------ */

#define USB_MSC_MAX_DEVICES  2     /* BLKDEV_MAX leaves room for ata + 2 */

struct msc_device {
    struct usb_device *dev;
    int                ep_in;
    int                ep_out;
    int                ep_max;
    int                in_toggle;
    int                out_toggle;
    uint32_t           tag_seq;
    /* Cached capacity (READ_CAPACITY result). */
    uint32_t           n_blocks;
    uint32_t           block_size;
    /* The blkdev struct that gets registered points at this. */
    struct blkdev      bdev;
};

static struct msc_device g_msc_devices[USB_MSC_MAX_DEVICES];
static int               g_n_msc_devices;

/* ---- Class request: Bulk-Only Mass Storage Reset --------------- */

static int msc_bulk_reset(struct msc_device *m, int iface) {
    /* USB MSC §3.1: class request 0xFF (Bulk-Only Mass Storage Reset),
     * direction OUT, recipient INTERFACE. Resets the device's BOT
     * state machine without disturbing the data toggles. */
    struct usb_setup_packet s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        .bRequest      = 0xFF,
        .wValue        = 0,
        .wIndex        = (uint16_t)iface,
        .wLength       = 0,
    };
    return m->dev->hc->control_transfer(m->dev->addr, m->dev->low_speed,
                                        m->dev->ep0_max_packet,
                                        &s, 0, 0, 0);
}

/* ---- BOT command driver ---------------------------------------- */

/* Send one CBW + (optional) data + receive CSW.
 *
 *   cb         pointer to the SCSI command block (1..16 bytes)
 *   cb_len     SCSI command length
 *   data       in/out data buffer (may be NULL if data_len == 0)
 *   data_len   data phase length
 *   data_in    1 if data phase is device→host; 0 if host→device
 *
 * Returns 0 on success (CSW status = 0), -1 on any error. */
static int bot_command(struct msc_device *m,
                       const uint8_t *cb, int cb_len,
                       void *data, int data_len, int data_in)
{
    uint8_t cbw[31];
    memset(cbw, 0, sizeof(cbw));
    /* dCBWSignature = "USBC" */
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    /* dCBWTag — a sequence number is fine, the device just echoes it. */
    uint32_t tag = ++m->tag_seq;
    cbw[4] = (uint8_t)(tag       );
    cbw[5] = (uint8_t)(tag >>  8 );
    cbw[6] = (uint8_t)(tag >> 16 );
    cbw[7] = (uint8_t)(tag >> 24 );
    /* dCBWDataTransferLength */
    cbw[8]  = (uint8_t)(data_len       );
    cbw[9]  = (uint8_t)(data_len >>  8 );
    cbw[10] = (uint8_t)(data_len >> 16 );
    cbw[11] = (uint8_t)(data_len >> 24 );
    /* bmCBWFlags */
    cbw[12] = data_in ? 0x80 : 0x00;
    /* bCBWLUN, bCBWCBLength */
    cbw[13] = 0;
    cbw[14] = (uint8_t)cb_len;
    /* CBWCB */
    for (int i = 0; i < cb_len && i < 16; i++) cbw[15 + i] = cb[i];

    /* Send CBW. */
    int rc = m->dev->hc->bulk_out(m->dev->addr, m->ep_max, m->ep_out,
                                  cbw, 31, &m->out_toggle);
    if (rc != 31) {
        kprintf("[msc] CBW send rc=%d (expected 31)\n", rc);
        return -1;
    }

    /* Data phase. */
    if (data_len > 0) {
        if (data_in) {
            rc = m->dev->hc->bulk_in(m->dev->addr, m->ep_max, m->ep_in,
                                     data, data_len, &m->in_toggle);
        } else {
            rc = m->dev->hc->bulk_out(m->dev->addr, m->ep_max, m->ep_out,
                                      data, data_len, &m->out_toggle);
        }
        if (rc < 0) {
            kprintf("[msc] data phase rc=%d (wanted %d)\n", rc, data_len);
            /* Fall through and still try to read CSW — the device
             * may be reporting a stall via CSW. */
        }
    }

    /* CSW. */
    uint8_t csw[13];
    rc = m->dev->hc->bulk_in(m->dev->addr, m->ep_max, m->ep_in,
                             csw, 13, &m->in_toggle);
    if (rc != 13) {
        kprintf("[msc] CSW recv rc=%d\n", rc);
        return -1;
    }

    if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S') {
        kprintf("[msc] CSW signature mismatch: %02x%02x%02x%02x\n",
                csw[0], csw[1], csw[2], csw[3]);
        return -1;
    }
    /* dCSWStatus at byte 12: 0 = passed, 1 = failed, 2 = phase error */
    if (csw[12] != 0) {
        kprintf("[msc] CSW status = %d\n", csw[12]);
        return -1;
    }
    return 0;
}

/* ---- SCSI commands -------------------------------------------- */

static int scsi_test_unit_ready(struct msc_device *m) {
    uint8_t cb[6] = {0x00, 0, 0, 0, 0, 0};
    return bot_command(m, cb, 6, 0, 0, 0);
}

static int scsi_inquiry(struct msc_device *m, uint8_t out[36]) {
    /* INQUIRY (6): opcode 0x12, allocation length = 36. */
    uint8_t cb[6] = {0x12, 0, 0, 0, 36, 0};
    return bot_command(m, cb, 6, out, 36, 1);
}

static int scsi_read_capacity10(struct msc_device *m,
                                 uint32_t *out_last_lba,
                                 uint32_t *out_block_size)
{
    /* READ CAPACITY (10): opcode 0x25, returns 8 bytes:
     *   [0..3] big-endian last LBA
     *   [4..7] big-endian block size */
    uint8_t cb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t buf[8];
    int rc = bot_command(m, cb, 10, buf, 8, 1);
    if (rc != 0) return rc;
    *out_last_lba   = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                    | ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
    *out_block_size = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16)
                    | ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];
    return 0;
}

static int scsi_read10(struct msc_device *m, uint32_t lba, uint16_t n,
                       void *buf)
{
    /* READ (10): opcode 0x28, big-endian 4-byte LBA at [2..5],
     * big-endian 2-byte transfer length (in blocks) at [7..8]. */
    uint8_t cb[10] = {
        0x28, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >>  8), (uint8_t) lba,
        0,
        (uint8_t)(n >> 8), (uint8_t) n,
        0
    };
    return bot_command(m, cb, 10, buf, n * m->block_size, 1);
}

static int scsi_write10(struct msc_device *m, uint32_t lba, uint16_t n,
                        const void *buf)
{
    /* WRITE (10): opcode 0x2A, layout identical to READ(10). */
    uint8_t cb[10] = {
        0x2A, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >>  8), (uint8_t) lba,
        0,
        (uint8_t)(n >> 8), (uint8_t) n,
        0
    };
    return bot_command(m, cb, 10, (void *)buf, n * m->block_size, 0);
}

/* ---- blkdev adapter ------------------------------------------- */

static int msc_blkdev_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf) {
    struct msc_device *m = (struct msc_device *)d->driver_data;
    if (n == 0) return 0;
    if (n > 0xFFFF) return -1;
    return scsi_read10(m, lba, (uint16_t)n, buf) == 0 ? 0 : -1;
}

static int msc_blkdev_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf) {
    struct msc_device *m = (struct msc_device *)d->driver_data;
    if (n == 0) return 0;
    if (n > 0xFFFF) return -1;
    return scsi_write10(m, lba, (uint16_t)n, buf) == 0 ? 0 : -1;
}

/* ---- Attach ---------------------------------------------------- */

void usb_msc_attach(struct usb_device *d,
                    int iface_num, int ep_in, int ep_out, int ep_max)
{
    if (g_n_msc_devices >= USB_MSC_MAX_DEVICES) {
        kprintf("[msc] no free MSC slot\n");
        return;
    }
    if (ep_max <= 0) ep_max = 64;

    struct msc_device *m = &g_msc_devices[g_n_msc_devices];
    memset(m, 0, sizeof(*m));
    m->dev        = d;
    m->ep_in      = ep_in;
    m->ep_out     = ep_out;
    m->ep_max     = ep_max;
    m->in_toggle  = 0;
    m->out_toggle = 0;
    m->tag_seq    = 0;

    /* Optional: bulk reset to start from a clean BOT state. Some
     * devices require it, most don't care. */
    msc_bulk_reset(m, iface_num);

    /* The first INQUIRY after bulk-reset can NAK a few times while
     * the device is settling; retry. */
    uint8_t inq[36];
    int rc = -1;
    for (int retries = 0; retries < 5; retries++) {
        rc = scsi_inquiry(m, inq);
        if (rc == 0) break;
        pit_sleep(20);
    }
    if (rc != 0) {
        kprintf("[msc] addr %d: INQUIRY failed\n", d->addr);
        return;
    }
    /* Vendor (8 bytes) + product (16 bytes) start at offset 8. */
    char vendor[9], product[17];
    for (int i = 0; i < 8;  i++) vendor[i]  = (char)inq[8 + i];
    for (int i = 0; i < 16; i++) product[i] = (char)inq[16 + i];
    vendor[8] = product[16] = 0;
    /* Trim trailing spaces — SCSI INQUIRY pads with 0x20. */
    for (int i = 7;  i >= 0 && vendor[i]  == ' '; i--) vendor[i]  = 0;
    for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;

    /* TEST UNIT READY can NAK while removable media spins up. */
    for (int retries = 0; retries < 5; retries++) {
        rc = scsi_test_unit_ready(m);
        if (rc == 0) break;
        pit_sleep(50);
    }

    uint32_t last_lba = 0, blksz = 0;
    if (scsi_read_capacity10(m, &last_lba, &blksz) != 0) {
        kprintf("[msc] addr %d: READ CAPACITY failed\n", d->addr);
        return;
    }
    m->n_blocks   = last_lba + 1;
    m->block_size = blksz;

    kprintf("[msc] addr %d  vendor=\"%s\"  product=\"%s\"\n",
            d->addr, vendor, product);
    kprintf("[msc] addr %d  capacity = %u blocks * %u B = %u KiB\n",
            d->addr, (unsigned)m->n_blocks, (unsigned)m->block_size,
            (unsigned)((m->n_blocks * m->block_size) / 1024));

    /* Register as a blkdev. */
    struct blkdev *b = &m->bdev;
    /* Name: "usb<n>" where n is the MSC index */
    b->name[0] = 'u'; b->name[1] = 's'; b->name[2] = 'b';
    b->name[3] = (char)('0' + g_n_msc_devices);
    b->name[4] = 0;
    b->block_size  = m->block_size;
    b->n_blocks    = m->n_blocks;
    b->read        = msc_blkdev_read;
    b->write       = msc_blkdev_write;
    b->driver_data = m;
    int idx = blkdev_register(b);
    if (idx < 0) {
        kprintf("[msc] blkdev_register full\n");
        return;
    }
    kprintf("[msc] registered as blkdev[%d] = %s\n", idx, b->name);
    g_n_msc_devices++;
}
