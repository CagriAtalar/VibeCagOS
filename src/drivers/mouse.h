/*
 * VibeCagOS — PS/2 Mouse Driver
 *
 * Supports standard 3-byte PS/2 mouse packets with X/Y delta and button state.
 */

#pragma once
#include "common.h"

struct mouse_state {
    int  x;
    int  y;
    bool left_btn;
    bool right_btn;
    bool middle_btn;
};

void mouse_init(int max_x, int max_y);
void mouse_handle_byte(uint8_t data);
void mouse_get_state(struct mouse_state *out);
void mouse_set_bounds(int max_x, int max_y);
