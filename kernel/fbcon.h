#ifndef ADVENTOS_FBCON_H
#define ADVENTOS_FBCON_H

#include "../include/types.h"

/* Framebuffer console: an 8x8-glyph text terminal that paints into the
 * VBE linear framebuffer. Drop-in for the VGA text mode console — use
 * fbcon_putc / fbcon_write the same way you'd use vga_putc.
 *
 * fbcon_init() reads vbe_state(); if the framebuffer is unavailable
 * (VBE off / mode-set failed in the bootloader) fbcon stays in a
 * disabled state and every entry point becomes a cheap no-op so
 * kprintf still works against serial + VGA. */

/* Common 24-bit colour packings. fbcon translates these to 16-/24-/
 * 32-bpp framebuffer pixels in pack_pixel(). */
#define FBC_BLACK       0x000000u
#define FBC_GREY        0xC0C0C0u
#define FBC_WHITE       0xFFFFFFu
#define FBC_RED         0xE03030u
#define FBC_GREEN       0x30E030u
#define FBC_BLUE        0x4080E0u
#define FBC_YELLOW      0xE0E030u
#define FBC_CYAN        0x30E0E0u
#define FBC_MAGENTA     0xE030E0u

void fbcon_init(void);
int  fbcon_enabled(void);

void fbcon_clear(void);
void fbcon_putc(char c);
void fbcon_write(const char *s);

void fbcon_set_color(uint32_t fg, uint32_t bg);

/* Direct primitive — handy for selftest demos and future GUI bits.
 * Coordinates clipped silently. */
void fbcon_fill_rect(uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h,
                     uint32_t color);

#endif
