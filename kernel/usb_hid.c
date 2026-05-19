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
#include "usb_hc.h"
#include "keyboard.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"
#include "task.h"
#include "mouse.h"
#include "vbe.h"

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
#define HID_MOD_LALT    (1 << 2)
#define HID_MOD_RALT    (1 << 6)

/* Session 135 — sentinel byte injected into the keyboard ring when
 * the user presses Alt+Tab.  Sits at 0x80 (outside printable ASCII
 * and outside any current keyboard-injection path) so wmd can
 * intercept it without confusing programs that read the ring for
 * normal text. */
#define KBD_ALT_TAB     0x80

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

/* Session 141 — USB tablet (HID class, non-boot protocol).  QEMU's
 * `-device usb-tablet` enumerates with bInterfaceProtocol = 0 and
 * delivers an 8-byte report-protocol report carrying absolute X / Y
 * in tablet logical units (0..32767 across the full display).
 * Bridging this to the kernel mouse state via mouse_set_absolute()
 * keeps the wmd-drawn cursor locked to QEMU's host pointer — no
 * grab/release, no PS/2-acceleration drift. */
struct hid_tablet {
    int           in_use;
    struct usb_device *dev;
    int           ep;
    int           ep_max;
    int           interval_ms;
    int           toggle;
};
#define MAX_HID_TABLET 2
static struct hid_tablet g_tablets[MAX_HID_TABLET];

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
    int alt   = (mods & (HID_MOD_LALT   | HID_MOD_RALT  )) ? 1 : 0;

    /* Session 135 — Alt+Tab bypasses the keyboard ring entirely.
     * The ring is a single-consumer queue and the shell would
     * happily eat the byte (or any other reader); send the press
     * via wm_post_alttab so wmd gets it deterministically the
     * same frame.  Raw HID Tab (without Alt) still falls through
     * to '\t' below. */
    if (alt && usage == 0x2B) {
        extern void wm_post_alttab(void);
        wm_post_alttab();
#ifdef USB_HID_TRACE
        kprintf("[usb-hid] Alt+Tab -> wm channel\n");
#endif
        return;
    }

    /* Arrow keys — emit the 3-byte ANSI CSI sequence (ESC '[' final),
     * same shape the PS/2 driver pushes via push_csi(). The shell
     * reads these in raw mode for history navigation; without this,
     * USB-HID users see arrow keys silently swallowed.
     *
     * HID usages:
     *   0x4F right -> CSI C
     *   0x50 left  -> CSI D
     *   0x51 down  -> CSI B
     *   0x52 up    -> CSI A    */
    if (usage >= 0x4F && usage <= 0x52) {
        static const char finals[4] = { 'C', 'D', 'B', 'A' };
        char esc[3] = { 27, '[', finals[usage - 0x4F] };
        keyboard_inject(esc, 3);
#ifdef USB_HID_TRACE
        kprintf("[usb-hid] arrow (usage=%x final=%c)\n", usage, esc[2]);
#endif
        return;
    }

    char c = shift ? hid_to_ascii_shifted[usage] : hid_to_ascii_unshifted[usage];
    if (!c) return;

    /* Ctrl-letter: standard ASCII control codes. */
    if (ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
    else if (ctrl && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);

    keyboard_inject(&c, 1);
    /* Session 82: the per-keystroke log corrupts the agent-facing
     * shell readability — every key emits a debug line interleaved
     * with the shell's character echo, making the agent-training-
     * surface "prompt + echo + output" contract unparseable. Gated
     * on -DUSB_HID_TRACE so kernel devs debugging the USB stack
     * can still see it, but the default agent-driven build is silent.
     * Same convention as SMP_TRACE (kernel/smp_trace.h, session 80). */
#ifdef USB_HID_TRACE
    kprintf("[usb-hid] '%c' (usage=%x mods=%x)\n", c, usage, mods);
#endif
}

static void poll_one(struct hid_kbd *k) {
    uint8_t report[8] = {0};
    int rc = k->dev->hc->int_in(k->dev->addr, k->dev->low_speed, k->ep_max,
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

/* Session 141 — parse one QEMU usb-tablet report.  Layout:
 *
 *   byte 0       : buttons (bit0=L, bit1=R, bit2=M, bit3..7 padding)
 *   bytes 1..2   : X (uint16 LE, logical 0..32767)
 *   bytes 3..4   : Y (uint16 LE, logical 0..32767)
 *   byte 5       : vertical wheel (signed 8-bit, ignored for now)
 *   bytes 6..7   : padding
 *
 * Logical 0..32767 spans the full display in each axis; scale to FB
 * dimensions and push into the kernel mouse state. */
static void poll_one_tablet(struct hid_tablet *t) {
    uint8_t report[8] = {0};
    /* Request 8 bytes but the QEMU usb-tablet only sends 6 (buttons +
     * X + Y + wheel).  uhci_int_in returns the actual byte count;
     * we only touch report[0..4] so a short read is fine. */
    int rc = t->dev->hc->int_in(t->dev->addr, t->dev->low_speed, t->ep_max,
                                t->ep, report, 8, &t->toggle);
    if (rc <= 0) return;     /* NAK / timeout = no new state */

    int buttons = report[0] & 0x07;
    int raw_x   = (int)report[1] | ((int)report[2] << 8);
    int raw_y   = (int)report[3] | ((int)report[4] << 8);

    const struct vbe_state *v = vbe_state();
    int fb_w = (v && v->enabled) ? (int)v->width  : 1024;
    int fb_h = (v && v->enabled) ? (int)v->height : 768;
    int x = (raw_x * (fb_w - 1)) / 32767;
    int y = (raw_y * (fb_h - 1)) / 32767;

    mouse_set_absolute(x, y, buttons);
#ifdef USB_HID_TRACE
    kprintf("[usb-hid] tablet raw=(%d,%d) -> (%d,%d) btns=%x\n",
            raw_x, raw_y, x, y, buttons);
#endif
}

static void usb_hid_tablet_task(void) {
    for (;;) {
        for (int i = 0; i < MAX_HID_TABLET; i++) {
            if (g_tablets[i].in_use) poll_one_tablet(&g_tablets[i]);
        }
        /* Faster than keyboard polling — a 15 ms loop gives ~66 Hz
         * cursor updates, smoother than the 30 fps WM frame rate. */
        pit_sleep(15);
    }
}

/* The HID boot-mouse path that lived here was removed when AdventOS
 * narrowed to a CLI-only OS for developers and AI agents. Only the
 * boot-keyboard path remains. If a HID interface enumerates with
 * USB_HID_PROTOCOL_MOUSE we just decline it. */

/* Called from usb_core's enumeration once it identifies a HID
 * interface.  Routes to keyboard (boot protocol) or tablet
 * (non-boot, absolute positioning).  Boot-protocol mice (relative)
 * remain declined — use usb-tablet instead. */
void usb_hid_attach(struct usb_device *d,
                    const uint8_t *cfg_buf, int cfg_len,
                    int iface_num, int proto, int ep_addr,
                    int ep_max, int ep_interval)
{
    (void)cfg_buf; (void)cfg_len; (void)ep_interval;

    if (proto == USB_HID_PROTOCOL_KEYBOARD) {
        /* Switch to boot protocol so we get the fixed-format 8-byte
         * keyboard report regardless of what the HID report descriptor
         * would otherwise mandate.  SET_IDLE(0) tells the device "only
         * report on change", suppressing duplicate idle-timeout
         * reports. */
        if (usb_hid_set_protocol(d, iface_num,
                                 USB_HID_PROTOCOL_BOOT) != USB_OK) {
            kprintf("[usb] addr %d: SET_PROTOCOL(BOOT) failed\n", d->addr);
            return;
        }
        usb_hid_set_idle(d, iface_num, 0);

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
        kprintf("[usb] HID boot-mouse ignored — use usb-tablet\n");
        return;
    }

    /* Session 141 — proto = 0 means "no boot protocol available",
     * which is how QEMU's usb-tablet advertises itself.  Register as
     * absolute-positioning tablet.  We deliberately do NOT issue
     * SET_PROTOCOL(BOOT) because the tablet doesn't support it;
     * report protocol is the default after SET_CONFIGURATION and is
     * what we want. */
    if (proto == 0) {
        struct hid_tablet *t = 0;
        for (int i = 0; i < MAX_HID_TABLET; i++) {
            if (!g_tablets[i].in_use) { t = &g_tablets[i]; break; }
        }
        if (!t) { kprintf("[usb] no free HID tablet slot\n"); return; }
        memset(t, 0, sizeof(*t));
        t->in_use      = 1;
        t->dev         = d;
        t->ep          = ep_addr;
        t->ep_max      = ep_max < 8 ? 8 : ep_max;
        t->interval_ms = ep_interval > 0 ? ep_interval : 10;
        t->toggle      = 0;
        kprintf("[usb] HID tablet registered "
                "(vid=%x pid=%x, polling starts late)\n",
                d->vendor_id, d->product_id);
        /* Session 144 — silence PS/2 deltas now so the cursor never
         * drifts via the PS/2 path while waiting for the first
         * tablet abs report. */
        mouse_set_tablet_active();
        return;
    }

    kprintf("[usb] HID interface (proto=%d) not recognized\n", proto);
}

void usb_start_polling(void);    /* fwd-decl, defined below */
void usb_start_polling(void) {
    int kbd_any = 0;
    int tablet_any = 0;
    for (int i = 0; i < MAX_HID_KBD; i++)
        if (g_kbds[i].in_use) kbd_any = 1;
    for (int i = 0; i < MAX_HID_TABLET; i++)
        if (g_tablets[i].in_use) tablet_any = 1;

    if (kbd_any) {
        task_make_runnable(task_create(usb_hid_kbd_task, "usb-hid-kbd"));
        kprintf("[usb] HID keyboard polling task started\n");
    }
    if (tablet_any) {
        task_make_runnable(task_create(usb_hid_tablet_task, "usb-hid-tablet"));
        kprintf("[usb] HID tablet polling task started\n");
    }
}
