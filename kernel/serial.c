#include "serial.h"
#include "isr.h"
#include "pic.h"
#include "../include/io.h"

#define BUF_SIZE 256

static volatile char     rx_buf[BUF_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;

static void serial_irq(struct registers *r) {
    (void)r;
    /* Drain everything available so we don't lose bytes when
     * many arrive between IRQs. */
    while (inb(COM1_PORT + 5) & 1) {
        char c = (char)inb(COM1_PORT);
        uint32_t next = (rx_head + 1) % BUF_SIZE;
        if (next != rx_tail) {
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
}

void serial_init(void) {
    rx_head = rx_tail = 0;

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
    /* Enable received-data-available interrupt */
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

int serial_buf_has_data(void) {
    return rx_head != rx_tail;
}

char serial_buf_pop(void) {
    if (rx_head == rx_tail) return 0;
    char c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % BUF_SIZE;
    return c;
}
