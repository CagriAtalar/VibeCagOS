/*
 * VibeCagOS — Graphical User Interface (GUI) & Window Manager
 *
 * Provides VGA Mode 13h (320x200 256-color) graphics, double-buffered
 * 2D drawing primitives, embedded bitmap font, desktop environment,
 * movable windows, mouse cursor, and clean text mode restore.
 */

#pragma once
#include "common.h"

/* Palette colors */
#define GUI_COLOR_BLACK       0
#define GUI_COLOR_BLUE        1
#define GUI_COLOR_GREEN       2
#define GUI_COLOR_CYAN        3
#define GUI_COLOR_RED         4
#define GUI_COLOR_MAGENTA     5
#define GUI_COLOR_BROWN       6
#define GUI_COLOR_LIGHTGRAY   7
#define GUI_COLOR_DARKGRAY    8
#define GUI_COLOR_LIGHTBLUE   9
#define GUI_COLOR_LIGHTGREEN  10
#define GUI_COLOR_LIGHTCYAN   11
#define GUI_COLOR_LIGHTRED    12
#define GUI_COLOR_LIGHTMAGENTA 13
#define GUI_COLOR_YELLOW      14
#define GUI_COLOR_WHITE       15

/* Screen dimensions */
#define GUI_WIDTH  320
#define GUI_HEIGHT 200

/* Drawing primitives */
void gui_clear(uint8_t color);
void gui_pixel(int x, int y, uint8_t color);
void gui_line(int x0, int y0, int x1, int y1, uint8_t color);
void gui_rect(int x, int y, int w, int h, uint8_t color);
void gui_rect_fill(int x, int y, int w, int h, uint8_t color);
void gui_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg);
void gui_draw_string(int x, int y, const char *s, uint8_t fg, uint8_t bg);
void gui_flip(void);

/* Window system */
struct gui_window {
    int  x, y, w, h;
    const char *title;
    uint8_t title_color;
    bool active;
};

void gui_draw_window(const struct gui_window *win);

/* Launch interactive GUI desktop environment (returns to text mode when user exits) */
void gui_launch_desktop(void);
