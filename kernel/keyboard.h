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

/* Session 160 — keyboard grab.  wmd calls keyboard_grab(pid) at
 * startup so it becomes the sole consumer of the kbd ring; any
 * other task that lands in keyboard_wait_char (e.g. the kernel-console
 * shell sitting at its prompt) yields indefinitely until wmd releases
 * the grab.  Without this, foreground/background races between wmd
 * and the outer shell mean keystrokes the user thinks are going to
 * wmterm sometimes get eaten by sh.elf's read_line_interactive. */
void keyboard_grab(int pid);
int  keyboard_grabbed_by(void);

#endif
