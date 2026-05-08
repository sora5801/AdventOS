#include "vga.h"
#include "string.h"
#include "../include/io.h"

#define VGA_BUFFER ((volatile uint16_t *)0xB8000)
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

static int     cursor_row;
static int     cursor_col;
static uint8_t cur_color;

static inline uint16_t make_cell(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

static void update_cursor_hw(void) {
    uint16_t pos = (uint16_t)(cursor_row * VGA_WIDTH + cursor_col);
    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    cur_color = (uint8_t)((bg << 4) | (fg & 0x0F));
}

void vga_clear(void) {
    uint16_t blank = make_cell(' ', cur_color);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = blank;
    }
    cursor_row = cursor_col = 0;
    update_cursor_hw();
}

static void scroll_if_needed(void) {
    if (cursor_row < VGA_HEIGHT) return;

    /* Move lines 1..H-1 up to 0..H-2 */
    for (int row = 0; row < VGA_HEIGHT - 1; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            VGA_BUFFER[row * VGA_WIDTH + col] =
                VGA_BUFFER[(row + 1) * VGA_WIDTH + col];
        }
    }
    /* Clear last line */
    uint16_t blank = make_cell(' ', cur_color);
    for (int col = 0; col < VGA_WIDTH; col++) {
        VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = blank;
    }
    cursor_row = VGA_HEIGHT - 1;
}

void vga_putc(char c) {
    switch (c) {
        case '\n':
            cursor_col = 0;
            cursor_row++;
            break;
        case '\r':
            cursor_col = 0;
            break;
        case '\t':
            cursor_col = (cursor_col + 4) & ~3;
            if (cursor_col >= VGA_WIDTH) {
                cursor_col = 0;
                cursor_row++;
            }
            break;
        case '\b':
            if (cursor_col > 0) {
                cursor_col--;
                VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] =
                    make_cell(' ', cur_color);
            }
            break;
        default:
            if ((uint8_t)c >= 32) {
                VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] =
                    make_cell(c, cur_color);
                cursor_col++;
                if (cursor_col >= VGA_WIDTH) {
                    cursor_col = 0;
                    cursor_row++;
                }
            }
            break;
    }
    scroll_if_needed();
    update_cursor_hw();
}

void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

void vga_set_cursor(int row, int col) {
    if (row >= 0 && row < VGA_HEIGHT) cursor_row = row;
    if (col >= 0 && col < VGA_WIDTH)  cursor_col = col;
    update_cursor_hw();
}

void vga_init(void) {
    cursor_row = cursor_col = 0;
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}
