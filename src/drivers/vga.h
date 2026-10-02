#pragma once

#include "common.h"

/* VGA color codes (foreground nibble, background nibble = 0x<bg><fg>) */
#define VGA_COLOR_BLACK         0
#define VGA_COLOR_BLUE          1
#define VGA_COLOR_GREEN         2
#define VGA_COLOR_CYAN          3
#define VGA_COLOR_RED           4
#define VGA_COLOR_MAGENTA       5
#define VGA_COLOR_BROWN         6
#define VGA_COLOR_LIGHT_GRAY    7
#define VGA_COLOR_DARK_GRAY     8
#define VGA_COLOR_LIGHT_BLUE    9
#define VGA_COLOR_LIGHT_GREEN   10
#define VGA_COLOR_LIGHT_CYAN    11
#define VGA_COLOR_LIGHT_RED     12
#define VGA_COLOR_LIGHT_MAGENTA 13
#define VGA_COLOR_YELLOW        14
#define VGA_COLOR_WHITE         15

/* Make color byte: bg << 4 | fg */
#define VGA_COLOR(fg, bg)  (((bg) << 4) | (fg))

/* Basic API */
void    vga_init(void);
void    vga_clear(void);
void    vga_putchar(char ch);
void    vga_puts(const char *str);
void    vga_set_color(uint8_t color);
uint8_t vga_get_color(void);

/* Drawing primitives */
void    vga_putchar_at(int col, int row, char ch, uint8_t color);
void    vga_fill_rect(int col, int row, int w, int h, char ch, uint8_t color);
