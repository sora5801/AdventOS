#include "keyboard.h"
#include "isr.h"
#include "pic.h"
#include "kprintf.h"
#include "serial.h"
#include "task.h"
#include "../include/io.h"

#define KBD_DATA_PORT 0x60
#define KBD_STATUS    0x64

#define BUF_SIZE 512   /* session-73 followup: was 128. Bumped to
                        * comfortably hold a clipboard paste of up
                        * to a few hundred chars without buf_push
                        * dropping bytes mid-burst. Each entry is one
                        * char so 512 bytes total — negligible. */

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

/* Scancode-set-1 → kbd ring. Pure logic, no I/O — split out from
 * kbd_irq so the polling fallback below can share the exact same
 * scancode handling. Session 68: needed because PIT/PS-2 IRQs don't
 * fire reliably on QEMU 10.x's i440fx + LAPIC chipset, so the shell
 * has to actively poll port 0x60 from keyboard_wait_char. */
static void process_scancode(uint8_t sc) {
    if (sc == 0xE0) { e0_prefix = 1; return; }
    if (sc == 0x2A || sc == 0x36) { shift_down = 1; e0_prefix = 0; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_down = 0; e0_prefix = 0; return; }
    if (sc == 0x3A) { caps_lock = !caps_lock; e0_prefix = 0; return; }
    if (sc & 0x80) { e0_prefix = 0; return; }

    if (e0_prefix) {
        e0_prefix = 0;
        switch (sc) {
            case 0x48: push_csi('A'); return;                   /* Up */
            case 0x50: push_csi('B'); return;                   /* Down */
            case 0x4D: push_csi('C'); return;                   /* Right */
            case 0x4B: push_csi('D'); return;                   /* Left */
            /* Session 161 — PageUp / PageDown emit the 4-byte
             * ANSI sequence ESC '[' 5 / 6 '~'.  wmterm's input-side
             * CSI parser intercepts these for scrollback; the
             * kernel console shell parses them as unknown CSI
             * (session 159 made sh tolerate parameter bytes) and
             * silently drops them.  This mirrors the same wiring
             * usb_hid.c does for USB keyboards. */
            case 0x49:                                          /* PageUp */
                buf_push(27); buf_push('[');
                buf_push('5'); buf_push('~');
                return;
            case 0x51:                                          /* PageDown */
                buf_push(27); buf_push('[');
                buf_push('6'); buf_push('~');
                return;
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

static void kbd_irq(struct registers *r) {
    (void)r;
    /* Session 68: gate on OBF before reading. Both the polling path
     * (keyboard_poll_once, driven by keyboard_wait_char) and this IRQ
     * handler now coexist; if polling drained the byte first, the
     * late-arriving IRQ would read a stale value from port 0x60 (QEMU
     * latches the last byte even after OBF clears) and double-push.
     * Checking OBF first makes whichever path got there first the
     * authoritative one; the other becomes a no-op. */
    uint8_t st = inb(KBD_STATUS);
    if (!(st & 0x01)) return;
    uint8_t b = inb(KBD_DATA_PORT);
    /* Session 109 — route AUX (mouse) bytes to the mouse driver. */
    if (st & 0x20) { extern void mouse_process_byte(uint8_t); mouse_process_byte(b); }
    else            process_scancode(b);
}

/* Drain everything currently in the i8042 output buffer. Safe to call
 * from any context (no locks, just PIO + the process_scancode logic).
 * Used by keyboard_wait_char as a polling fallback because PIT and
 * PS/2 IRQs don't fire under our QEMU 10.x + i440fx setup. The OBF
 * flag is bit 0 of the status port (0x64); we use the literal here
 * so this function is independent of KBD_STATUS_OBF which is defined
 * further down in this file.
 *
 * Session 109 — for each byte, check status bit 5 (AUX) and route
 * mouse bytes to mouse_process_byte instead of feeding them into the
 * scancode decoder. */
void keyboard_poll_once(void) {
    extern void mouse_process_byte(uint8_t);
    uint8_t st;
    while ((st = inb(KBD_STATUS)) & 0x01) {
        uint8_t b = inb(KBD_DATA_PORT);
        if (st & 0x20) mouse_process_byte(b);
        else            process_scancode(b);
    }
}

/* i8042 PS/2 controller programming.
 *
 * Session 68 — pre-this, keyboard_init() only unmasked the PIC line and
 * drained the buffer, banking on SeaBIOS leaving the i8042 in a state
 * where IRQ 1 fires on keypress. Under QEMU 10.1 that assumption breaks:
 * sendkey from the monitor queues a scancode into the i8042 but the
 * controller never raises IRQ 1, so the kernel sees nothing. Verified
 * by tracing irq_handler (added in this session) — zero `[irq] 1`
 * lines after a sendkey, despite the i8042 being present in info qtree
 * with kbd-irq=1.
 *
 * Standard init sequence (OSDev wiki, "8042 PS/2 Controller"):
 *   1. Disable both ports so config writes don't race incoming bytes
 *   2. Flush the output buffer
 *   3. Read the configuration byte (command 0x20)
 *   4. Set bit 0 (kbd IRQ enable) and clear bit 4 (kbd clock disable);
 *      leave the other bits — bit 6 (translation Set 1 → Set 1) is
 *      already what we want under QEMU's default
 *   5. Write the modified config back (command 0x60)
 *   6. Re-enable the keyboard port (command 0xAE)
 *   7. Enable scanning on the keyboard device (data 0xF4)
 *
 * On QEMU this completes in microseconds; we use simple polling with
 * a generous spin cap to stay defensive against a misbehaving i8042. */

#define KBD_STATUS_OBF  0x01    /* output buffer full (CPU can read 0x60) */
#define KBD_STATUS_IBF  0x02    /* input  buffer full (CPU must wait) */

static void kbd_wait_input_empty(void) {
    /* Wait until IBF clears so we can safely write to 0x60 / 0x64. */
    for (int i = 0; i < 100000; i++) {
        if (!(inb(KBD_STATUS) & KBD_STATUS_IBF)) return;
    }
}

static void kbd_wait_output_full(void) {
    /* Wait until OBF is set so we can safely read from 0x60. */
    for (int i = 0; i < 100000; i++) {
        if (inb(KBD_STATUS) & KBD_STATUS_OBF) return;
    }
}

void keyboard_init(void) {
    kbd_head = kbd_tail = 0;
    shift_down = caps_lock = 0;
    e0_prefix = 0;

    /* --- i8042 programming --- */

    /* 1. Disable both ports (kbd port 1, mouse port 2). */
    kbd_wait_input_empty();
    outb(KBD_STATUS, 0xAD);     /* disable port 1 */
    kbd_wait_input_empty();
    outb(KBD_STATUS, 0xA7);     /* disable port 2 (no-op if absent) */

    /* 2. Flush output buffer. */
    while (inb(KBD_STATUS) & KBD_STATUS_OBF) (void)inb(KBD_DATA_PORT);

    /* 3. Read config byte. */
    kbd_wait_input_empty();
    outb(KBD_STATUS, 0x20);
    kbd_wait_output_full();
    uint8_t cfg = inb(KBD_DATA_PORT);

    /* 4. Enable kbd IRQ (bit 0), clear kbd clock-disable (bit 4).
     *    Leave bit 1 (mouse IRQ) and bit 5 (mouse clock disable)
     *    alone — we don't drive the mouse, and toggling them might
     *    confuse a hypervisor that's emulating both halves. */
    cfg |=  0x01;
    cfg &= ~0x10;

    /* 5. Write config back. */
    kbd_wait_input_empty();
    outb(KBD_STATUS, 0x60);
    kbd_wait_input_empty();
    outb(KBD_DATA_PORT, cfg);

    /* 6. Re-enable kbd port. */
    kbd_wait_input_empty();
    outb(KBD_STATUS, 0xAE);

    /* 7. Enable scanning on the keyboard device itself (0xF4 to data
     *    port). The keyboard ACKs with 0xFA, which the IRQ handler
     *    will swallow once interrupts are enabled — we just shovel
     *    it out and move on. */
    kbd_wait_input_empty();
    outb(KBD_DATA_PORT, 0xF4);

    /* Drain whatever's in the output buffer post-init (typically the
     * 0xFA ACK from the scanning-enable command). */
    while (inb(KBD_STATUS) & KBD_STATUS_OBF) (void)inb(KBD_DATA_PORT);

    /* --- IRQ wiring --- */
    isr_register_irq(1, kbd_irq);
    pic_clear_mask(1);

    kprintf("[kbd] PS/2 ready: IRQ1 unmasked, i8042 cfg=0x%x\n",
            (unsigned)cfg);
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

/* Session 160 — keyboard grab state.  wmd calls keyboard_grab(pid)
 * at startup so it becomes the sole consumer; keyboard_wait_char
 * yields all other tasks until grab is cleared.  task_destroy in
 * kernel/task.c clears this if the grabber dies without releasing. */
static int g_kbd_grab_pid = 0;

char keyboard_wait_char(void) {
    /* Session 68: actively poll both input paths each iteration.
     *
     * Under QEMU 10.x + i440fx + LAPIC enabled, neither PIT IRQ 0 nor
     * PS/2 IRQ 1 fires reliably after boot — the legacy 8259 INT line
     * stops routing to LAPIC LINT0 once SVR.bit8 goes high, and even
     * with SVR.bit8 cleared the chipset routing remains broken in our
     * Windows/MSYS2 QEMU build. So we can't depend on `sti; hlt` ever
     * being woken by a keyboard or timer IRQ.
     *
     * The fix: spin-poll port 0x60 (PS/2 OBF) and the COM1 LSR each
     * iteration. Any keypress queued by QEMU's i8042 emulator or any
     * byte typed into the host terminal (-serial stdio) gets pulled
     * into the kbd ring within microseconds. Cost: this CPU stays at
     * ~100% while the shell is waiting for input — acceptable for a
     * single-user interactive shell on a developer system.
     *
     * A `pause` instruction tells the CPU we're in a spin loop so it
     * can deprioritize this hyperthread (no-op on single-threaded
     * cores but cheap insurance). */
    /* Session 80: yield each iteration so we don't pin the BKL while
     * the shell is idle waiting for input. The original session-68
     * design (pure spin) was fine under -smp 1 because no other CPU
     * needed the BKL; under -smp 2 a kernel task on the peer CPU
     * (reaper, bcache_syncer, an IRQ-driven socket consumer) will
     * eventually call bkl_lock() and deadlock waiting for us, since
     * syscall_dispatch holds the BKL across the entire SYS_READ.
     *
     * task_yield is bkl-aware: drops BKL across the schedule(), then
     * reacquires. The schedule() typically returns to us immediately
     * via the keep-prev branch (no other runnable on this CPU), but
     * the BKL drop in between is the whole point — it lets the peer
     * CPU's kernel task acquire it, do its work, release. */
    extern void task_yield(void);
    extern struct task *task_current(void);
    for (;;) {
        keyboard_poll_once();
        serial_poll_once();

        /* Session 160 — if some other task has grabbed the keyboard
         * (typically wmd), don't compete for the ring; yield until
         * the grab is released.  Without this the outer shell at its
         * raw-mode prompt races wmd for every keystroke and `wmterm &`
         * users see typing eaten by sh's read_line_interactive instead
         * of arriving at the focused WM client. */
        struct task *t = task_current();
        if (g_kbd_grab_pid != 0 && t && (int)t->id != g_kbd_grab_pid) {
            task_yield();
            continue;
        }

        if (kbd_head != kbd_tail) {
            char c = kbd_buf[kbd_tail];
            kbd_tail = (kbd_tail + 1) % BUF_SIZE;
            return c;
        }
        task_yield();
    }
}

void keyboard_grab(int pid) {
    g_kbd_grab_pid = pid;
}

int keyboard_grabbed_by(void) {
    return g_kbd_grab_pid;
}
