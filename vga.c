#include "common.h"

// VGA text mode buffer at 0xB8000
#define VGA_MEMORY 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_COLOR 0x0F  // White on black

static uint16_t *vga_buffer = (uint16_t *)VGA_MEMORY;
static int vga_col = 0;
static int vga_row = 0;

void vga_init(void) {
    // Clear screen
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = (VGA_COLOR << 8) | ' ';
    }
    vga_col = 0;
    vga_row = 0;
}

static void vga_scroll(void) {
    // Move all rows up
    for (int row = 0; row < VGA_HEIGHT - 1; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            vga_buffer[row * VGA_WIDTH + col] = vga_buffer[(row + 1) * VGA_WIDTH + col];
        }
    }
    // Clear last row
    for (int col = 0; col < VGA_WIDTH; col++) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = (VGA_COLOR << 8) | ' ';
    }
    vga_row = VGA_HEIGHT - 1;
}

void vga_putchar(char ch) {
    if (ch == '\n') {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_HEIGHT) {
            vga_scroll();
        }
    } else if (ch == '\r') {
        vga_col = 0;
    } else if (ch == '\b') {
        if (vga_col > 0) {
            vga_col--;
            vga_buffer[vga_row * VGA_WIDTH + vga_col] = (VGA_COLOR << 8) | ' ';
        }
    } else {
        vga_buffer[vga_row * VGA_WIDTH + vga_col] = (VGA_COLOR << 8) | ch;
        vga_col++;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
            if (vga_row >= VGA_HEIGHT) {
                vga_scroll();
            }
        }
    }
}

void vga_puts(const char *str) {
    while (*str) {
        vga_putchar(*str++);
    }
}

