/*
 * VibeCagOS — VGA Text Mode Driver
 *
 * Supports 80x25 VGA text mode with color attributes.
 * Colors use the standard 4-bit VGA color scheme:
 *   0x0F = white text, black background
 *   0x0A = bright green text, black background
 *   0x0C = bright red text, black background
 *   etc.
 */

#include "common.h"
#include "vga.h"

#define VGA_MEMORY  ((uint16_t *)0xB8000)
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

#define VGA_DEFAULT_COLOR 0x07  /* Light gray on black */

static uint16_t *vga_buf = (uint16_t *)0xB8000;
static int       vga_col = 0;
static int       vga_row = 0;
static uint8_t   vga_color = VGA_DEFAULT_COLOR;

/* Hardware cursor ports */
#define VGA_CRTC_ADDR 0x3D4
#define VGA_CRTC_DATA 0x3D5

static inline void outb_vga(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void vga_update_cursor(void) {
    uint16_t pos = (uint16_t)(vga_row * VGA_WIDTH + vga_col);
    outb_vga(VGA_CRTC_ADDR, 14);
    outb_vga(VGA_CRTC_DATA, (uint8_t)(pos >> 8));
    outb_vga(VGA_CRTC_ADDR, 15);
    outb_vga(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
}

void vga_init(void) {
    vga_color = VGA_DEFAULT_COLOR;
    vga_clear();
}

void vga_clear(void) {
    uint16_t blank = (uint16_t)((vga_color << 8) | ' ');
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga_buf[i] = blank;
    vga_col = 0;
    vga_row = 0;
    vga_update_cursor();
}

void vga_set_color(uint8_t color) {
    vga_color = color;
}

uint8_t vga_get_color(void) {
    return vga_color;
}

static void vga_scroll(void) {
    /* Move all rows up by one */
    for (int row = 0; row < VGA_HEIGHT - 1; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            vga_buf[row * VGA_WIDTH + col] =
                vga_buf[(row + 1) * VGA_WIDTH + col];
        }
    }
    /* Clear last row */
    uint16_t blank = (uint16_t)((vga_color << 8) | ' ');
    for (int col = 0; col < VGA_WIDTH; col++)
        vga_buf[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = blank;
    vga_row = VGA_HEIGHT - 1;
}

void vga_putchar(char ch) {
    if (ch == '\n') {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_HEIGHT)
            vga_scroll();
    } else if (ch == '\r') {
        vga_col = 0;
    } else if (ch == '\t') {
        /* Tab = advance to next 8-column boundary */
        int next = (vga_col + 8) & ~7;
        while (vga_col < next && vga_col < VGA_WIDTH) {
            vga_buf[vga_row * VGA_WIDTH + vga_col] =
                (uint16_t)((vga_color << 8) | ' ');
            vga_col++;
        }
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
            if (vga_row >= VGA_HEIGHT)
                vga_scroll();
        }
    } else if (ch == '\b') {
        if (vga_col > 0) {
            vga_col--;
            vga_buf[vga_row * VGA_WIDTH + vga_col] =
                (uint16_t)((vga_color << 8) | ' ');
        }
    } else {
        vga_buf[vga_row * VGA_WIDTH + vga_col] =
            (uint16_t)((vga_color << 8) | (unsigned char)ch);
        vga_col++;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
            if (vga_row >= VGA_HEIGHT)
                vga_scroll();
        }
    }
    vga_update_cursor();
}

void vga_puts(const char *str) {
    while (*str)
        vga_putchar(*str++);
}

void vga_putchar_at(int col, int row, char ch, uint8_t color) {
    if (col < 0 || col >= VGA_WIDTH || row < 0 || row >= VGA_HEIGHT)
        return;
    vga_buf[row * VGA_WIDTH + col] = (uint16_t)((color << 8) | (unsigned char)ch);
}

void vga_fill_rect(int col, int row, int w, int h, char ch, uint8_t color) {
    uint16_t cell = (uint16_t)((color << 8) | (unsigned char)ch);
    for (int r = row; r < row + h && r < VGA_HEIGHT; r++) {
        for (int c = col; c < col + w && c < VGA_WIDTH; c++) {
            vga_buf[r * VGA_WIDTH + c] = cell;
        }
    }
}
