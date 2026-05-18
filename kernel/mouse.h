#ifndef ADVENTOS_MOUSE_H
#define ADVENTOS_MOUSE_H

#include "../include/types.h"

/* PS/2 mouse driver. Session 109 — Path C phase 3.
 *
 * Programmed via the i8042 controller (ports 0x60 / 0x64). Bytes
 * arrive interleaved with keyboard data on port 0x60; the keyboard
 * polling path (kernel/keyboard.c) checks i8042 status bit 5 (AUX)
 * before each read and routes mouse bytes here via
 * mouse_process_byte() rather than into the scancode decoder.
 *
 * State is accumulated as absolute (x, y) clamped to the VBE
 * framebuffer dimensions, plus a 3-bit button mask. Userspace reads
 * the snapshot via SYS_MOUSE_POLL (in syscall.c).
 *
 * Like the keyboard driver, mouse IRQ 12 is unreliable on the
 * current QEMU + Windows MSYS2 setup; the kernel relies on the
 * keyboard's polling drain — which now routes mouse bytes too — to
 * pick up packets at human-interactive rates. */

void mouse_init(void);

/* Called from the i8042 drain loop for each byte where status bit 5
 * was set (AUX = mouse). Assembles three-byte packets and updates
 * the accumulated state on every third byte. */
void mouse_process_byte(uint8_t b);

/* Read the accumulated state. x and y are absolute screen
 * coordinates (clamped to fb dims at update time). `buttons` is a
 * 3-bit mask: bit 0 = left, bit 1 = right, bit 2 = middle. */
void mouse_get_state(int *x_out, int *y_out, int *buttons_out);

/* Session 141 — absolute pointer override.  USB tablet reports
 * carry absolute X / Y (in tablet logical units 0..32767 over
 * the full display).  The HID driver scales those to FB pixels
 * and calls this; subsequent PS/2 packets keep updating the
 * same state but tablet reports overwrite both deltas and
 * absolute positions on each tick.  Buttons is a 3-bit mask. */
void mouse_set_absolute(int x, int y, int buttons);

#endif
