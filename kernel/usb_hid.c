/*
 * USB HID boot keyboard driver.
 *
 * The HID 1.11 spec describes the *report* descriptor parser for
 * arbitrary HID devices, but the spec also defines a fixed-format
 * "boot protocol" for keyboards (and mice) that any host can drive
 * without a report-descriptor parser. We use boot protocol — that
 * means an 8-byte report every poll:
 *
 *   byte 0  : modifier bitmap (LCtrl, LShift, LAlt, LGUI,
 *                              RCtrl, RShift, RAlt, RGUI — bits 0..7)
 *   byte 1  : reserved (always 0)
 *   bytes 2-7: up to 6 simultaneously-pressed HID Usage codes
 *
 * The polling task does an INTERRUPT-IN transfer every bInterval
 * ms (10 ms by default for QEMU's usb-kbd), compares the report
 * to the previous one, and injects ASCII for each *newly* pressed
 * key into the existing keyboard ring buffer. Existing PS/2 code
 * paths (TTY, shell line editor, raw-mode reads) all read from
 * that ring without caring whether a key arrived from PS/2 or USB.
 *
 * Because we run as a kernel task with `pit_sleep` between polls,
 * USB keystroke latency is one PIT tick (10 ms) plus one USB frame
 * (1 ms). Imperceptible.
 */
#include "usb.h"
#include "usb_core.h"
#include "uhci.h"
#include "keyboard.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"
#include "task.h"

/* HID Usage IDs (USB HID Usage Tables §10) → ASCII. Indexed by
 * Usage ID 0..0x73. Two parallel tables for unshifted/shifted. */

#define HID_USAGE_MAX  0x68

static const char hid_to_ascii_unshifted[HID_USAGE_MAX] = {
    /* 0x00 */ 0,    0,    0,    0,
    /* 0x04 */ 'a','b','c','d','e','f','g','h','i','j','k','l','m',
    /* 0x11 */ 'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 0x1E */ '1','2','3','4','5','6','7','8','9','0',
    /* 0x28 */ '\n', 0x1B /*esc*/, '\b', '\t', ' ',
    /* 0x2D */ '-', '=', '[', ']', '\\', 0,
    /* 0x33 */ ';', '\'', '`', ',', '.', '/',
    /* 0x39 */ 0,                                  /* CapsLock */
    /* 0x3A..0x45 — F1..F12 — not mapped to ASCII */
    [0x3A] = 0, [0x3B] = 0, [0x3C] = 0, [0x3D] = 0,
    [0x3E] = 0, [0x3F] = 0, [0x40] = 0, [0x41] = 0,
    [0x42] = 0, [0x43] = 0, [0x44] = 0, [0x45] = 0,
    /* 0x4F..0x52 — arrows */
    [0x4F] = 0, [0x50] = 0, [0x51] = 0, [0x52] = 0,
};

static const char hid_to_ascii_shifted[HID_USAGE_MAX] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
    [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
    [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
    [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Y', [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*',
    [0x26] = '(', [0x27] = ')',
    [0x28] = '\n', [0x29] = 0x1B, [0x2A] = '\b', [0x2B] = '\t',
    [0x2C] = ' ',
    [0x2D] = '_',  [0x2E] = '+',  [0x2F] = '{',  [0x30] = '}',
    [0x31] = '|',  [0x33] = ':',  [0x34] = '"',  [0x35] = '~',
    [0x36] = '<',  [0x37] = '>',  [0x38] = '?',
};

#define HID_MOD_LSHIFT  (1 << 1)
#define HID_MOD_RSHIFT  (1 << 5)
#define HID_MOD_LCTRL   (1 << 0)
#define HID_MOD_RCTRL   (1 << 4)

/* Per-device state. We expect ≤1 keyboard for the demo, but the
 * code is general (one struct per attached HID device). */
struct hid_kbd {
    int           in_use;
    struct usb_device *dev;
    int           ep;
    int           ep_max;
    int           interval_ms;
    int           toggle;
    uint8_t       prev[8];
};

#define MAX_HID_KBD 2
static struct hid_kbd g_kbds[MAX_HID_KBD];

/* Look up usage in the new report; return 1 if the usage was
 * present, 0 otherwise. */
static int report_contains(const uint8_t r[8], uint8_t usage) {
    for (int i = 2; i < 8; i++) if (r[i] == usage) return 1;
    return 0;
}

static void emit_for_usage(uint8_t usage, uint8_t mods) {
    if (usage >= HID_USAGE_MAX) return;
    int shift = (mods & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) ? 1 : 0;
    int ctrl  = (mods & (HID_MOD_LCTRL  | HID_MOD_RCTRL )) ? 1 : 0;

    char c = shift ? hid_to_ascii_shifted[usage] : hid_to_ascii_unshifted[usage];
    if (!c) return;

    /* Ctrl-letter: standard ASCII control codes. */
    if (ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
    else if (ctrl && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);

    keyboard_inject(&c, 1);
    /* Per-keystroke log makes it obvious in serial output that a
     * USB key (vs a PS/2 key) reached us. Quiet enough for normal
     * typing — eight bytes per press. */
    kprintf("[usb-hid] '%c' (usage=%x mods=%x)\n", c, usage, mods);
}

static void poll_one(struct hid_kbd *k) {
    uint8_t report[8] = {0};
    int rc = uhci_int_in(k->dev->addr, k->dev->low_speed, k->ep_max,
                         k->ep, report, 8, &k->toggle);
    if (rc <= 0) return;     /* NAK / timeout / error: just retry next tick */

    /* For each key in the NEW report not in the previous report,
     * synthesize a press. (Boot keyboard reports are level-triggered
     * — every poll sees the full set of currently-down keys — so
     * we diff to detect edges.) */
    uint8_t mods = report[0];
    for (int i = 2; i < 8; i++) {
        uint8_t u = report[i];
        if (u == 0 || u == 1) continue;     /* unused / rollover */
        if (!report_contains(k->prev, u))   emit_for_usage(u, mods);
    }
    for (int i = 0; i < 8; i++) k->prev[i] = report[i];
}

static void usb_hid_kbd_task(void) {
    for (;;) {
        for (int i = 0; i < MAX_HID_KBD; i++) {
            if (g_kbds[i].in_use) poll_one(&g_kbds[i]);
        }
        /* 50 ms gives the rest of the system breathing room.
         * QEMU usb-kbd reports a 10 ms bInterval but in practice
         * 50 ms is plenty for a kernel keystroke and avoids
         * dominating CPU when nothing's happening. */
        pit_sleep(50);
    }
}

/* The HID boot-mouse path that lived here was removed when AdventOS
 * narrowed to a CLI-only OS for developers and AI agents. Only the
 * boot-keyboard path remains. If a HID interface enumerates with
 * USB_HID_PROTOCOL_MOUSE we just decline it. */

/* Called from usb_core's enumeration once it identifies a HID
 * interface. Routes to keyboard; ignores mice. */
void usb_hid_attach(struct usb_device *d,
                    const uint8_t *cfg_buf, int cfg_len,
                    int iface_num, int proto, int ep_addr,
                    int ep_max, int ep_interval)
{
    (void)cfg_buf; (void)cfg_len; (void)ep_interval;

    /* Switch to boot protocol so we get the fixed-format reports
     * regardless of what the HID report descriptor would otherwise
     * mandate. SET_IDLE(0) tells the device "only report on change",
     * suppressing duplicate idle-timeout reports. */
    if (usb_hid_set_protocol(d, iface_num, USB_HID_PROTOCOL_BOOT) != USB_OK) {
        kprintf("[usb] addr %d: SET_PROTOCOL(BOOT) failed\n", d->addr);
        return;
    }
    usb_hid_set_idle(d, iface_num, 0);

    if (proto == USB_HID_PROTOCOL_KEYBOARD) {
        struct hid_kbd *k = 0;
        for (int i = 0; i < MAX_HID_KBD; i++) {
            if (!g_kbds[i].in_use) { k = &g_kbds[i]; break; }
        }
        if (!k) { kprintf("[usb] no free HID kbd slot\n"); return; }
        memset(k, 0, sizeof(*k));
        k->in_use      = 1;
        k->dev         = d;
        k->ep          = ep_addr;
        k->ep_max      = ep_max < 8 ? 8 : ep_max;
        k->interval_ms = ep_interval > 0 ? ep_interval : 10;
        k->toggle      = 0;
        kprintf("[usb] HID keyboard registered (polling starts late)\n");
        return;
    }

    if (proto == USB_HID_PROTOCOL_MOUSE) {
        kprintf("[usb] HID mouse ignored — AdventOS is CLI-only\n");
        return;
    }

    kprintf("[usb] HID interface (proto=%d) not recognized\n", proto);
}

void usb_start_polling(void);    /* fwd-decl, defined below */
void usb_start_polling(void) {
    int kbd_any = 0;
    for (int i = 0; i < MAX_HID_KBD; i++)
        if (g_kbds[i].in_use) kbd_any = 1;

    if (kbd_any) {
        task_make_runnable(task_create(usb_hid_kbd_task, "usb-hid-kbd"));
        kprintf("[usb] HID keyboard polling task started\n");
    }
}
