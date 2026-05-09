#include "keyboard.h"
#include "isr.h"
#include "pic.h"
#include "serial.h"
#include "../include/io.h"

#define KBD_DATA_PORT 0x60
#define KBD_STATUS    0x64

#define BUF_SIZE 128

static volatile char     kbd_buf[BUF_SIZE];
static volatile uint32_t kbd_head;
static volatile uint32_t kbd_tail;

static int shift_down;
static int caps_lock;

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

static void kbd_irq(struct registers *r) {
    (void)r;
    uint8_t sc = inb(KBD_DATA_PORT);

    /* Modifier keys */
    if (sc == 0x2A || sc == 0x36) { shift_down = 1; return; }   /* shift down */
    if (sc == 0xAA || sc == 0xB6) { shift_down = 0; return; }   /* shift up   */
    if (sc == 0x3A) { caps_lock = !caps_lock; return; }         /* caps lock  */

    if (sc & 0x80) return;            /* ignore other key releases */

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
    for (;;) {
        /* Serial input doubles as a console for headless QEMU usage. */
        if (serial_buf_has_data()) {
            char c = serial_buf_pop();
            if (c == '\r') c = '\n';
            if (c == 127) c = '\b';
            return c;
        }
        if (kbd_head != kbd_tail) {
            char c = kbd_buf[kbd_tail];
            kbd_tail = (kbd_tail + 1) % BUF_SIZE;
            return c;
        }
        __asm__ volatile ("sti; hlt");
    }
}
