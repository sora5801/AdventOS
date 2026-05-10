#include "mouse.h"
#include "isr.h"
#include "pic.h"
#include "kprintf.h"
#include "vbe.h"
#include "../include/io.h"

/* ---- PS/2 controller registers ----------------------------------- */
#define PS2_DATA      0x60
#define PS2_STATUS    0x64    /* read */
#define PS2_COMMAND   0x64    /* write */

/* PS/2 status register bits */
#define PS2_STATUS_OUTPUT_FULL  0x01    /* data available to read */
#define PS2_STATUS_INPUT_FULL   0x02    /* controller is busy reading */
#define PS2_STATUS_AUX_DATA     0x20    /* the byte in DATA came from mouse,
                                           not keyboard */

/* PS/2 controller commands */
#define PS2_CMD_DISABLE_AUX     0xA7    /* disable mouse port */
#define PS2_CMD_ENABLE_AUX      0xA8    /* enable mouse port  */
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_WRITE_AUX       0xD4    /* "next byte to DATA goes to mouse" */

/* Mouse commands (sent via PS2_CMD_WRITE_AUX prefix) */
#define MOUSE_CMD_RESET         0xFF
#define MOUSE_CMD_SET_DEFAULTS  0xF6
#define MOUSE_CMD_ENABLE_REPORT 0xF4
#define MOUSE_ACK               0xFA

/* ---- driver state ------------------------------------------------ */
static volatile int32_t  g_x;
static volatile int32_t  g_y;
static volatile uint32_t g_buttons;
static volatile uint32_t g_packets;
static int32_t           g_screen_w = 1024;
static int32_t           g_screen_h = 768;

/* 3-byte packet assembler. cycle = 0/1/2 indexes the byte we expect
 * next. Resync on a byte 0 with the always-1 sync bit clear. */
static volatile uint8_t  g_pkt[3];
static volatile uint8_t  g_pkt_cycle;

static int g_initialized;

/* ---- low-level helpers ------------------------------------------- */

/* Wait until the controller's input buffer is empty so it'll accept
 * the next command/data byte. Bounded loop — a stuck controller
 * shouldn't deadlock the boot. */
static int ps2_wait_input(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_INPUT_FULL)) return 0;
    }
    return -1;
}

/* Wait until the controller has output for us. */
static int ps2_wait_output(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) return 0;
    }
    return -1;
}

static void ps2_send_command(uint8_t cmd) {
    ps2_wait_input();
    outb(PS2_COMMAND, cmd);
}

static void ps2_send_data(uint8_t b) {
    ps2_wait_input();
    outb(PS2_DATA, b);
}

static int ps2_read_data(uint8_t *out) {
    if (ps2_wait_output() < 0) return -1;
    *out = inb(PS2_DATA);
    return 0;
}

/* Send a byte to the mouse (vs the keyboard) — every mouse byte
 * needs to be prefixed with controller command 0xD4 telling the
 * controller "the next thing on PS2_DATA is for the aux device". */
static int mouse_write(uint8_t b) {
    ps2_send_command(PS2_CMD_WRITE_AUX);
    ps2_send_data(b);
    /* Most mouse commands ack with 0xFA. We tolerate timeout — some
     * BIOSes / virtualized stacks omit the ack on certain commands. */
    uint8_t ack;
    if (ps2_read_data(&ack) == 0 && ack == MOUSE_ACK) return 0;
    return -1;
}

/* ---- IRQ handler ------------------------------------------------- */

static void mouse_irq(struct registers *r) {
    (void)r;
    uint8_t status = inb(PS2_STATUS);
    if (!(status & PS2_STATUS_OUTPUT_FULL)) return;
    /* Bit 5 = "this byte came from the AUX device". If it's the
     * keyboard, leave it alone — the keyboard ISR will eat it on its
     * own IRQ1. (We're sharing the controller, not the IRQ.) */
    if (!(status & PS2_STATUS_AUX_DATA))    return;

    uint8_t b = inb(PS2_DATA);

    /* Resync: the first byte of every packet has bit 3 set ("always
     * 1"). If we're at cycle 0 and the byte doesn't have it, the
     * stream got out of phase — drop the byte and stay at cycle 0
     * until we see one with bit 3 set. */
    if (g_pkt_cycle == 0 && !(b & 0x08)) return;

    g_pkt[g_pkt_cycle++] = b;
    if (g_pkt_cycle < 3) return;
    g_pkt_cycle = 0;

    uint8_t flags = g_pkt[0];

    /* Drop overflowed packets — the dx/dy values are unreliable. */
    if (flags & 0xC0) return;

    /* Sign-extend dx/dy out of the 9-bit-signed encoding. */
    int32_t dx = g_pkt[1];
    int32_t dy = g_pkt[2];
    if (flags & 0x10) dx |= 0xFFFFFF00;     /* X-sign */
    if (flags & 0x20) dy |= 0xFFFFFF00;     /* Y-sign */

    /* Mouse Y is "up = positive"; FB Y is "down = positive". Invert. */
    int32_t nx = g_x + dx;
    int32_t ny = g_y - dy;

    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > g_screen_w - 1) nx = g_screen_w - 1;
    if (ny > g_screen_h - 1) ny = g_screen_h - 1;

    g_x = nx;
    g_y = ny;
    g_buttons = (uint32_t)(flags & 0x07);
    g_packets++;
}

/* ---- public API -------------------------------------------------- */

void mouse_get_state(struct mouse_state *out) {
    if (!out) return;
    /* Cheap snapshot — torn reads ok for a UI cursor. */
    out->x       = g_x;
    out->y       = g_y;
    out->buttons = g_buttons;
    out->packets = g_packets;
}

void mouse_set_pos(int32_t x, int32_t y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > g_screen_w - 1) x = g_screen_w - 1;
    if (y > g_screen_h - 1) y = g_screen_h - 1;
    g_x = x;
    g_y = y;
}

void mouse_init(void) {
    if (g_initialized) return;

    /* Pick clamp box from the framebuffer if VBE came up; otherwise
     * stick with the 1024x768 defaults. */
    const struct vbe_state *v = vbe_state();
    if (v && v->enabled) {
        g_screen_w = (int32_t)v->width;
        g_screen_h = (int32_t)v->height;
    }
    /* Spawn the cursor in the middle. */
    g_x = g_screen_w / 2;
    g_y = g_screen_h / 2;

    /* Drain any stale bytes the BIOS or earlier code left in the
     * controller's output buffer. */
    for (int i = 0; i < 16 && (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL); i++) {
        (void)inb(PS2_DATA);
    }

    /* 1. Enable the mouse port (controller cmd 0xA8). */
    ps2_send_command(PS2_CMD_ENABLE_AUX);

    /* 2. Read the controller config byte, set bit 1 (enable IRQ12
     *    for AUX) and clear bit 5 (don't disable AUX clock). Some
     *    QEMU configurations init the byte with bit 5 set. */
    ps2_send_command(PS2_CMD_READ_CONFIG);
    uint8_t cfg = 0;
    if (ps2_read_data(&cfg) < 0) {
        kprintf("mouse: controller config read timed out — disabled\n");
        return;
    }
    cfg |=  (1u << 1);    /* enable AUX IRQ */
    cfg &= ~(1u << 5);    /* enable AUX clock */
    ps2_send_command(PS2_CMD_WRITE_CONFIG);
    ps2_send_data(cfg);

    /* 3. Tell the mouse to use defaults (sample rate 100Hz, scaling
     *    1:1, 3-byte packets, reporting disabled). */
    if (mouse_write(MOUSE_CMD_SET_DEFAULTS) != 0) {
        kprintf("mouse: SET_DEFAULTS not ack'd — assuming no mouse, disabled\n");
        return;
    }

    /* 4. Enable data reporting. After this, IRQ12 fires on every
     *    movement / button event. */
    if (mouse_write(MOUSE_CMD_ENABLE_REPORT) != 0) {
        kprintf("mouse: ENABLE_REPORT not ack'd — disabled\n");
        return;
    }

    /* 5. Wire the IRQ. IRQ12 is on the slave PIC; cascade IRQ2 was
     *    unmasked by kmain at PIC remap time, so unmasking 12 here
     *    is enough to start delivery. */
    isr_register_irq(12, mouse_irq);
    pic_clear_mask(12);

    g_initialized = 1;
    kprintf("mouse: PS/2 ready, clamp box %dx%d\n",
            (int)g_screen_w, (int)g_screen_h);
}
