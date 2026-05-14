#include "keyboard.h"
#include "isr.h"
#include "pic.h"
#include "../include/io.h"

#define KBD_DATA_PORT 0x60
#define KBD_STATUS    0x64

#define BUF_SIZE 128

static volatile char     kbd_buf[BUF_SIZE];
static volatile uint32_t kbd_head;
static volatile uint32_t kbd_tail;

static int shift_down;
static int caps_lock;

/* Session 49: track 0xE0 prefix for "extended" scancodes.
 * The dedicated arrow cluster, Home/End/Ins/Del, and the right-side
 * Ctrl/Alt all send 0xE0 followed by their normal scancode. We use
 * this to distinguish the arrow cluster from the numpad (which sends
 * the same raw scancodes — 0x48/0x4B/0x4D/0x50 — without prefix). */
static int e0_prefix;

/* Scancode set 1 -> ASCII, lowercase */
static const char scancode_lower[128] = {
    /* 0x00 */  0,    27,  '1', '2', '3', '4', '5', '6',
    /* 0x08 */ '7',  '8', '9', '0', '-', '=', '\b','\t',
    /* 0x10 */ 'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 0x18 */ 'o',  'p', '[', ']', '\n', 0,  'a', 's',
    /* 0x20 */ 'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 0x28 */ '\'', '`',  0, '\\', 'z', 'x', 'c', 'v',
    /* 0x30 */ 'b',  'n', 'm', ',', '.', '/',  0,  '*',
    /* 0x38 */  0,   ' ',  0,   0,   0,   0,   0,   0,
    /* 0x40 */  0,    0,   0,   0,   0,   0,   0,  '7',
    /* 0x48 */ '8',  '9', '-', '4', '5', '6', '+', '1',
    /* 0x50 */ '2',  '3', '0', '.', 0,   0,   0,   0,
    /* 0x58 */  0,    0,   0,   0,   0,   0,   0,   0,
    /* 0x60..0x7F all zero */
};

static const char scancode_upper[128] = {
    /* 0x00 */  0,    27,  '!', '@', '#', '$', '%', '^',
    /* 0x08 */ '&',  '*', '(', ')', '_', '+', '\b','\t',
    /* 0x10 */ 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 0x18 */ 'O',  'P', '{', '}', '\n', 0,  'A', 'S',
    /* 0x20 */ 'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
    /* 0x28 */ '"',  '~',  0, '|', 'Z', 'X', 'C', 'V',
    /* 0x30 */ 'B',  'N', 'M', '<', '>', '?',  0,  '*',
    /* 0x38 */  0,   ' ',  0,   0,   0,   0,   0,   0,
    /* 0x40 */  0,    0,   0,   0,   0,   0,   0,  '7',
    /* 0x48 */ '8',  '9', '-', '4', '5', '6', '+', '1',
    /* 0x50 */ '2',  '3', '0', '.', 0,   0,   0,   0,
};

static void buf_push(char c) {
    uint32_t next = (kbd_head + 1) % BUF_SIZE;
    if (next == kbd_tail) return;     /* drop on overflow */
    kbd_buf[kbd_head] = c;
    kbd_head = next;
}

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Push a 3-byte ANSI CSI sequence: ESC '[' <final>.
 * For arrow keys: A=up, B=down, C=right, D=left. The shell parses
 * these in raw mode to drive history navigation. */
static void push_csi(char final) {
    buf_push(27);           /* ESC */
    buf_push('[');
    buf_push(final);
}

static void kbd_irq(struct registers *r) {
    (void)r;
    uint8_t sc = inb(KBD_DATA_PORT);

    /* 0xE0 is a one-byte prefix marking the next scancode as "extended"
     * (right-side modifiers, dedicated arrow cluster, etc). Latch it
     * and wait for the next byte. */
    if (sc == 0xE0) { e0_prefix = 1; return; }

    /* Modifier keys (both halves of the key set the same shift flag). */
    if (sc == 0x2A || sc == 0x36) { shift_down = 1; e0_prefix = 0; return; }   /* shift down */
    if (sc == 0xAA || sc == 0xB6) { shift_down = 0; e0_prefix = 0; return; }   /* shift up   */
    if (sc == 0x3A) { caps_lock = !caps_lock; e0_prefix = 0; return; }         /* caps lock  */

    if (sc & 0x80) { e0_prefix = 0; return; }   /* ignore other key releases */

    /* Extended scancodes (after 0xE0). We only care about the arrow
     * cluster — emit ANSI CSI sequences for them, drop everything else. */
    if (e0_prefix) {
        e0_prefix = 0;
        switch (sc) {
            case 0x48: push_csi('A'); return;   /* up arrow    */
            case 0x50: push_csi('B'); return;   /* down arrow  */
            case 0x4D: push_csi('C'); return;   /* right arrow */
            case 0x4B: push_csi('D'); return;   /* left arrow  */
            /* TODO: Home/End/Del/PgUp/PgDn would go here. */
            default: return;
        }
    }

    char c = scancode_lower[sc & 0x7F];
    if (!c) return;

    if (is_letter(c)) {
        int upper = shift_down ^ caps_lock;
        if (upper) c = scancode_upper[sc & 0x7F];
    } else if (shift_down) {
        char u = scancode_upper[sc & 0x7F];
        if (u) c = u;
    }

    buf_push(c);
}

void keyboard_init(void) {
    kbd_head = kbd_tail = 0;
    shift_down = caps_lock = 0;
    e0_prefix = 0;

    isr_register_irq(1, kbd_irq);
    pic_clear_mask(1);

    /* Drain any pending byte */
    while (inb(KBD_STATUS) & 1) (void)inb(KBD_DATA_PORT);
}

int keyboard_has_char(void) {
    return kbd_head != kbd_tail;
}

char keyboard_getc(void) {
    if (kbd_head == kbd_tail) return 0;
    char c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % BUF_SIZE;
    return c;
}

void keyboard_inject(const char *bytes, int n) {
    for (int i = 0; i < n; i++) buf_push(bytes[i]);
}

char keyboard_wait_char(void) {
    /* Session 67: the standalone serial poll branch that used to live
     * here is gone. The COM1 IRQ now feeds the kbd ring directly via
     * serial_inject_bytes -> keyboard_inject, so there's exactly one
     * place to wait — the shared kbd ring. PS/2, USB-HID, COM1, and
     * SYS_TTY_INJECT all funnel into the same queue. */
    for (;;) {
        if (kbd_head != kbd_tail) {
            char c = kbd_buf[kbd_tail];
            kbd_tail = (kbd_tail + 1) % BUF_SIZE;
            return c;
        }
        __asm__ volatile ("sti; hlt");
    }
}
