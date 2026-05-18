/*
 * mouse.c — PS/2 mouse driver (session 109, Path C phase 3).
 *
 * The i8042 controller multiplexes keyboard + mouse on the same data
 * port (0x60). When a byte appears, status bit 5 (AUX) tells us
 * whether it came from the mouse; the keyboard polling path in
 * kernel/keyboard.c calls mouse_process_byte() for those rather
 * than feeding them into the scancode decoder.
 *
 * Packet format (3-byte standard packet):
 *   byte 0: [Y_OVF | X_OVF | Y_SIGN | X_SIGN | 1 | BTN_MID | BTN_RIGHT | BTN_LEFT]
 *   byte 1: X delta (9-bit signed with sign in byte 0)
 *   byte 2: Y delta (same)
 *
 * Notes:
 * - PS/2 Y is up-positive; screen Y is down-positive — we invert.
 * - byte 0 bit 3 is always 1 on a real packet; if cleared, we resync
 *   by discarding the byte (next byte becomes "byte 0" again).
 * - Overflow bits are observed but ignored (treat as the clamped
 *   max-delta value the hardware already gave us).
 */

#include "mouse.h"
#include "io.h"
#include "kprintf.h"
#include "vbe.h"

#define KBD_DATA      0x60
#define KBD_STATUS    0x64

#define STATUS_OBF    0x01
#define STATUS_IBF    0x02
#define STATUS_AUX    0x20    /* set on the byte at 0x60 if AUX/mouse */

#define CMD_DISABLE_MOUSE  0xA7
#define CMD_ENABLE_MOUSE   0xA8
#define CMD_READ_CONFIG    0x20
#define CMD_WRITE_CONFIG   0x60
#define CMD_WRITE_TO_MOUSE 0xD4

#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_ENABLE_REPORT 0xF4

static int     g_initialized;
static int     g_mouse_x;
static int     g_mouse_y;
static int     g_buttons;     /* low 3 bits = L|R|M */

static uint8_t g_pkt[3];
static int     g_pkt_idx;

static void wait_input_empty(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(KBD_STATUS) & STATUS_IBF)) return;
    }
}
static void wait_output_full(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(KBD_STATUS) & STATUS_OBF) return;
    }
}

/* Send a single-byte command to the mouse and swallow its 0xFA ACK. */
static void send_mouse_cmd(uint8_t cmd) {
    wait_input_empty();
    outb(KBD_STATUS, CMD_WRITE_TO_MOUSE);
    wait_input_empty();
    outb(KBD_DATA, cmd);
    wait_output_full();
    (void)inb(KBD_DATA);     /* ACK (0xFA expected) */
}

void mouse_init(void) {
    /* 1. Enable the AUX (mouse) port. */
    wait_input_empty();
    outb(KBD_STATUS, CMD_ENABLE_MOUSE);

    /* 2. Read the i8042 config byte. */
    wait_input_empty();
    outb(KBD_STATUS, CMD_READ_CONFIG);
    wait_output_full();
    uint8_t cfg = inb(KBD_DATA);

    /* 3. Enable mouse IRQ12 (bit 1) + clear mouse-clock-disable (bit 5). */
    cfg |=  0x02;
    cfg &= ~0x20;

    /* 4. Write config back. */
    wait_input_empty();
    outb(KBD_STATUS, CMD_WRITE_CONFIG);
    wait_input_empty();
    outb(KBD_DATA, cfg);

    /* 5. Mouse defaults + enable streaming. */
    send_mouse_cmd(MOUSE_SET_DEFAULTS);
    send_mouse_cmd(MOUSE_ENABLE_REPORT);

    /* Drain whatever leftover bytes the init left. */
    for (int i = 0; i < 16; i++) {
        if (!(inb(KBD_STATUS) & STATUS_OBF)) break;
        (void)inb(KBD_DATA);
    }

    /* Start the cursor at screen center. */
    const struct vbe_state *v = vbe_state();
    if (v && v->enabled) {
        g_mouse_x = (int)v->width  / 2;
        g_mouse_y = (int)v->height / 2;
    } else {
        g_mouse_x = 512;
        g_mouse_y = 384;
    }
    g_buttons = 0;
    g_pkt_idx = 0;
    g_initialized = 1;

    kprintf("[mouse] PS/2 mouse ready (cursor centered at %d,%d)\n",
            g_mouse_x, g_mouse_y);
}

void mouse_process_byte(uint8_t b) {
    if (!g_initialized) return;

    /* Resync on byte 0: bit 3 must be 1. If not, drop and stay at idx 0. */
    if (g_pkt_idx == 0 && !(b & 0x08)) {
        return;
    }

    g_pkt[g_pkt_idx++] = b;
    if (g_pkt_idx < 3) return;
    g_pkt_idx = 0;

    /* Decode 9-bit deltas. byte0 X_SIGN → 0x100 bit of dx; similarly Y. */
    int dx = (int)g_pkt[1] - ((g_pkt[0] << 4) & 0x100);
    int dy = (int)g_pkt[2] - ((g_pkt[0] << 3) & 0x100);

    g_mouse_x += dx;
    g_mouse_y -= dy;     /* invert — PS/2 Y is up-positive */
    g_buttons  = (int)(g_pkt[0] & 0x07);

    const struct vbe_state *v = vbe_state();
    int max_x = (v && v->enabled) ? (int)v->width  - 1 : 1023;
    int max_y = (v && v->enabled) ? (int)v->height - 1 : 767;
    if (g_mouse_x < 0)     g_mouse_x = 0;
    if (g_mouse_y < 0)     g_mouse_y = 0;
    if (g_mouse_x > max_x) g_mouse_x = max_x;
    if (g_mouse_y > max_y) g_mouse_y = max_y;
}

void mouse_get_state(int *x_out, int *y_out, int *buttons_out) {
    if (x_out)       *x_out = g_mouse_x;
    if (y_out)       *y_out = g_mouse_y;
    if (buttons_out) *buttons_out = g_buttons;
}

/* Session 141 — USB tablet absolute-positioning entry point.
 * Called from kernel/usb_hid.c's tablet polling task.  No PS/2
 * packet bookkeeping — just overwrite the position. */
void mouse_set_absolute(int x, int y, int buttons) {
    const struct vbe_state *v = vbe_state();
    int max_x = (v && v->enabled) ? (int)v->width  - 1 : 1023;
    int max_y = (v && v->enabled) ? (int)v->height - 1 : 767;
    if (x < 0)     x = 0;
    if (y < 0)     y = 0;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;
    g_mouse_x = x;
    g_mouse_y = y;
    g_buttons = buttons & 0x07;
}
