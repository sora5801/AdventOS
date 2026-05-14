#include "serial.h"
#include "keyboard.h"
#include "isr.h"
#include "pic.h"
#include "../include/io.h"

/*
 * 16550 UART driver — TX (kprintf path) since session 1, RX added /
 * re-wired in session 67.
 *
 * RX shape: an IRQ on COM1 (PIC line 4) drains every byte the FIFO
 * is currently holding. Each byte is translated at the driver
 * boundary so callers above the kbd layer see the same byte shape
 * regardless of which physical input fed them:
 *
 *     0x0D ('\r')  →  0x0A ('\n')     terminals send CR; our TTY
 *                                     line-discipline + sh both
 *                                     expect LF as the line break
 *     0x7F (DEL)   →  0x08 ('\b')     terminals send DEL on
 *                                     backspace; the existing TTY
 *                                     edit + scancode-set-1 path
 *                                     expect 0x08
 *     0x03 (^C)    →  passes through  so a future TTY ISIG layer
 *                                     can deliver SIGINT to the
 *                                     foreground pgrp (today the
 *                                     console TTY doesn't do that
 *                                     — the byte just lands in the
 *                                     kbd ring like any other —
 *                                     but session 56's PTY layer
 *                                     and any future console-TTY
 *                                     work hinge on this byte
 *                                     reaching kbd unmodified)
 *
 * After translation each byte goes through `keyboard_inject`, the
 * same entry point the PS/2 driver and the USB-HID boot keyboard
 * path use. Everything above this — the kbd ring, sys_read on
 * fd 0, sh's raw-mode line editor — works identically regardless
 * of which keyboard the byte came in from. That's the whole point
 * of consolidating the input at this layer.
 *
 * Pre-session-67 this driver had a secondary rx_buf ring and a
 * keyboard_wait_char polling fallback. That worked under the very
 * specific "blocking canonical reader" path but missed every other
 * consumer (raw-mode sys_read, sys_kbd_poll, etc), and the bytes
 * sat in rx_buf in limbo until somebody happened to call
 * keyboard_wait_char. The new shape is a strict pipeline: bytes
 * arrive in the kbd ring as soon as the IRQ services them.
 */

static char translate_for_kbd(char c) {
    if (c == '\r')         return '\n';
    if (c == 0x7F)         return '\b';
    return c;
}

/* The IRQ-equivalent entry point used both by serial_irq and by the
 * SYS_SERIAL_INJECT selftest path. Splitting it out lets the
 * selftest exercise the exact same code without faking a UART read. */
void serial_inject_bytes(const char *bytes, int n) {
    for (int i = 0; i < n; i++) {
        char c = translate_for_kbd(bytes[i]);
        keyboard_inject(&c, 1);
    }
}

static void serial_irq(struct registers *r) {
    (void)r;
    /* Drain everything the FIFO is currently holding. The 16550 RX
     * FIFO is 14 bytes deep in the threshold we configured, but a
     * fast typist or a pasted line can deliver more than that
     * between IRQs — looping until LSR bit 0 clears handles both. */
    while (inb(COM1_PORT + 5) & 1) {
        char raw = (char)inb(COM1_PORT);
        char c   = translate_for_kbd(raw);
        keyboard_inject(&c, 1);
    }
}

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);   /* Disable interrupts */
    outb(COM1_PORT + 3, 0x80);   /* DLAB on */
    outb(COM1_PORT + 0, 0x03);   /* Divisor LSB: 38400 baud */
    outb(COM1_PORT + 1, 0x00);   /* Divisor MSB */
    outb(COM1_PORT + 3, 0x03);   /* 8N1, DLAB off */
    outb(COM1_PORT + 2, 0xC7);   /* FIFO enable, clear, 14-byte threshold */
    outb(COM1_PORT + 4, 0x0B);   /* IRQs enabled at MCR (RTS/DSR/OUT2) */
}

void serial_install_irq(void) {
    isr_register_irq(COM1_IRQ, serial_irq);
    pic_clear_mask(COM1_IRQ);
    /* Enable received-data-available interrupt. */
    outb(COM1_PORT + 1, 0x01);
}

static int tx_empty(void) { return inb(COM1_PORT + 5) & 0x20; }
static int rx_ready(void) { return inb(COM1_PORT + 5) & 0x01; }

void serial_putc(char c) {
    if (c == '\n') {
        while (!tx_empty()) {}
        outb(COM1_PORT, '\r');
    }
    while (!tx_empty()) {}
    outb(COM1_PORT, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

int serial_has_data(void) {
    return rx_ready();
}

char serial_getc(void) {
    while (!rx_ready()) {}
    return (char)inb(COM1_PORT);
}
