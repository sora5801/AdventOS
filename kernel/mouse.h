#ifndef ADVENTOS_MOUSE_H
#define ADVENTOS_MOUSE_H

#include "../include/types.h"

/* PS/2 mouse driver — IRQ 12 (slave PIC line 4).
 *
 * The PS/2 mouse controller streams 3-byte packets:
 *   byte 0: flags (Y-overflow:7 X-overflow:6 Y-sign:5 X-sign:4
 *                   always_1:3 middle:2 right:1 left:0)
 *   byte 1: dx (signed; sign extended from flags.X-sign)
 *   byte 2: dy (signed; sign extended from flags.Y-sign;
 *                NOTE: in screen coordinates +Y is DOWN, but the
 *                mouse sends +dy when moving UP. We invert in the
 *                IRQ handler so user coordinates match the FB.)
 *
 * The driver maintains an (x, y) cursor clamped to a configurable
 * screen size, and a bitmask of currently-held buttons. Userspace
 * reads the snapshot via SYS_MOUSE_STATE.
 *
 * If the BIOS / QEMU model doesn't expose a PS/2 mouse, mouse_init
 * gives up cleanly and SYS_MOUSE_STATE just returns the initial
 * (zero) state forever — the user GUI handles "no mouse" gracefully. */

#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04

struct mouse_state {
    int32_t  x;          /* 0..(screen_w-1) */
    int32_t  y;          /* 0..(screen_h-1) */
    uint32_t buttons;    /* bitmask of MOUSE_BTN_* */
    uint32_t packets;    /* total packets received (= move events count) */
};

/* Bring up the PS/2 mouse + IRQ12 handler. Reads vbe_state() to know
 * the screen dimensions for cursor clamping; if VBE isn't enabled,
 * defaults to a reasonable 1024x768 box. Idempotent. Logs progress
 * via kprintf — the BIOS handshake is several command/data round-
 * trips and a misbehaving controller should be loud about it. */
void               mouse_init(void);

/* Snapshot the current cursor + button state. Caller is racing the
 * IRQ but we only ever update three int32 fields — torn reads are
 * fine for a UI cursor. */
void               mouse_get_state(struct mouse_state *out);

/* Set the cursor position from kernel/user code (e.g., GUI program
 * teleporting the cursor to a click target). Clamped to screen.
 * Doesn't affect button state. */
void               mouse_set_pos(int32_t x, int32_t y);

/* Apply a delta + new button state. Used by the USB HID boot-mouse
 * polling task (session 44) so a USB mouse plugged in via a hub
 * shows up in the same `mouse_state` snapshot sys_mouse_state reads. */
void               mouse_inject(int32_t dx, int32_t dy, uint32_t buttons);

/* Force absolute (x, y, btns) — session 57 SYS_MOUSE_INJECT. The GUI
 * selftest uses this to land the cursor on a known window-content
 * pixel without depending on PS/2 packet arrival timing. */
void               mouse_set_state(int x, int y, int buttons);

#endif
