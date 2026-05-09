#ifndef ADVENTOS_TTY_H
#define ADVENTOS_TTY_H

#include "../include/types.h"

/*
 * Tiny TTY layer. Sits between SYS_READ on fd 0 and the keyboard /
 * serial input drivers. Exposes two modes — POSIX-flavored —
 * controlled by a per-TTY flag bitmap.
 *
 * Cooked / canonical mode (the historical default): the kernel
 * line-edits, echoes characters as the user types, treats backspace,
 * and only delivers a complete line to the program when Enter is
 * pressed. This is what kshell_read_line has done since session 11.
 *
 * Raw mode: every keystroke is delivered to the program as soon as
 * it arrives. No line editing, no buffering past the kernel input
 * ring. Echo is independently configurable. This is what an editor
 * or curses-style program needs to drive the screen one keystroke
 * at a time.
 *
 * There is one global TTY in this kernel — every user task's fd 0
 * shares it. A future per-task or per-pty model would replace this
 * single g_mode with a per-tty struct.
 */

#define TTY_ICANON   0x01    /* canonical (line-buffered) mode */
#define TTY_ECHO     0x02    /* echo input characters back to console */

#define TTY_DEFAULT  (TTY_ICANON | TTY_ECHO)
#define TTY_RAW      0u                          /* no canon, no echo */

void     tty_init(void);

uint32_t tty_get_mode(void);
void     tty_set_mode(uint32_t flags);

/* The unified read entry point — used by SYS_READ on fd 0. Honors
 * the current mode: canonical → line-buffered (delegates to
 * kshell_read_line); raw → returns up to `cap` bytes, blocking for
 * at least one. */
int      tty_read(char *buf, int cap);

/* Push raw bytes into the keyboard input ring as if they had been
 * typed. Used by SYS_TTY_INJECT so the headless boot's selftest can
 * feed the raw-mode reader without needing real keyboard input. */
int      tty_inject(const char *bytes, int n);

/* Foreground process group (session 20). The shell calls
 * tty_set_fg_pgrp(pipeline_pgid) before waiting for a foreground
 * pipeline so signals from the controlling terminal (Ctrl-C / Ctrl-Z
 * — ISIG path, deferred) would be routed to the pipeline rather
 * than the shell. Today only SYS_TCSETPGRP / SYS_TCGETPGRP touch it;
 * the value defaults to 0 (no foreground pgrp). */
uint32_t tty_get_fg_pgrp(void);
void     tty_set_fg_pgrp(uint32_t pgid);

#endif
