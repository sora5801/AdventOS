#ifndef ADVENTOS_VGA_H
#define ADVENTOS_VGA_H

#include "../include/types.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* Standard VGA text-mode colors */
#define VGA_BLACK         0x0
#define VGA_BLUE          0x1
#define VGA_GREEN         0x2
#define VGA_CYAN          0x3
#define VGA_RED           0x4
#define VGA_MAGENTA       0x5
#define VGA_BROWN         0x6
#define VGA_LIGHT_GREY    0x7
#define VGA_DARK_GREY     0x8
#define VGA_LIGHT_BLUE    0x9
#define VGA_LIGHT_GREEN   0xA
#define VGA_LIGHT_CYAN    0xB
#define VGA_LIGHT_RED     0xC
#define VGA_LIGHT_MAGENTA 0xD
#define VGA_YELLOW        0xE
#define VGA_WHITE         0xF

void vga_init(void);
void vga_clear(void);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_putc(char c);
void vga_write(const char *s);
void vga_set_cursor(int row, int col);

#endif
