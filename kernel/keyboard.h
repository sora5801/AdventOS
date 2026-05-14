#ifndef ADVENTOS_KEYBOARD_H
#define ADVENTOS_KEYBOARD_H

#include "../include/types.h"

void keyboard_init(void);

/* Returns 0 if no key is buffered, otherwise the next ASCII char (or special). */
char keyboard_getc(void);
int  keyboard_has_char(void);

/* Blocks until a key arrives (uses HLT between checks). */
char keyboard_wait_char(void);

/* Push raw bytes into the keyboard input ring as if they had been
 * typed. Used by the TTY layer's SYS_TTY_INJECT to drive raw-mode
 * tests without needing an actual keyboard / serial peer. */
void keyboard_inject(const char *bytes, int n);

/* Session 68: drain the PS/2 controller's output buffer in polling
 * mode. Same scancode-set-1 processing as the IRQ-driven path. Safe
 * to call from any context — no locks, just port I/O. Used by
 * keyboard_wait_char to actively pull keypresses when IRQ 1 isn't
 * firing (QEMU 10.x / chipset-routing issue). */
void keyboard_poll_once(void);

#endif
