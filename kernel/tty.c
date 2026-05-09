#include "tty.h"
#include "keyboard.h"
#include "shell.h"
#include "kprintf.h"

/* Single global mode for now — there's one console pair across the
 * whole system. A future PTY layer would make this per-tty and
 * reach it via fd 0's underlying tty handle. */
static volatile uint32_t g_mode = TTY_DEFAULT;

void tty_init(void) {
    g_mode = TTY_DEFAULT;
}

uint32_t tty_get_mode(void) {
    return g_mode;
}

void tty_set_mode(uint32_t flags) {
    /* Only the bits we model are honored; everything else is ignored. */
    g_mode = flags & (TTY_ICANON | TTY_ECHO);
}

int tty_read(char *buf, int cap) {
    if (cap <= 0) return 0;

    if (g_mode & TTY_ICANON) {
        /* Canonical: defer to the line-edited reader. It echoes,
         * handles backspace, and returns when Enter is pressed.
         * The TTY_ECHO flag is conceptually ignored here — canonical
         * input always echoes today. */
        return kshell_read_line(buf, cap);
    }

    /* Raw: block for at least one byte, then drain whatever else is
     * already available. Return immediately. */
    int n = 0;
    char c = keyboard_wait_char();
    buf[n++] = c;
    while (n < cap && keyboard_has_char()) {
        buf[n++] = keyboard_getc();
    }

    /* Echo if requested (default is OFF in raw mode — programs that
     * want it set TTY_ECHO explicitly). */
    if (g_mode & TTY_ECHO) {
        for (int i = 0; i < n; i++) kputc(buf[i]);
    }
    return n;
}

int tty_inject(const char *bytes, int n) {
    if (n <= 0) return 0;
    keyboard_inject(bytes, n);
    return n;
}
