/*
 * VibeCagOS — PS/2 Mouse Driver Implementation
 */

#include "mouse.h"
#include "kernel.h"
#include "klog.h"

static struct mouse_state current_state = { 160, 100, false, false, false };
static int bound_max_x = 320;
static int bound_max_y = 200;

static uint8_t mouse_cycle = 0;
static uint8_t mouse_bytes[3];

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        /* Wait to read */
        while (timeout--) {
            if ((inb(0x64) & 1) == 1) return;
        }
    } else {
        /* Wait to write */
        while (timeout--) {
            if ((inb(0x64) & 2) == 0) return;
        }
    }
}

static void mouse_write(uint8_t write) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, write);
}

static uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

void mouse_init(int max_x, int max_y) {
    bound_max_x = max_x;
    bound_max_y = max_y;
    current_state.x = max_x / 2;
    current_state.y = max_y / 2;

    /* Enable auxiliary mouse device */
    mouse_wait(1);
    outb(0x64, 0xA8);

    /* Enable mouse interrupts in controller config */
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    uint8_t status = inb(0x60) | 2;
    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);

    /* Use default settings */
    mouse_write(0xF6);
    mouse_read(); /* ACK */

    /* Enable packet streaming */
    mouse_write(0xF4);
    mouse_read(); /* ACK */

    KINFO("MOUSE", "PS/2 Mouse initialized (bounds: %dx%d)", max_x, max_y);
}

void mouse_handle_byte(uint8_t data) {
    switch (mouse_cycle) {
        case 0:
            if ((data & 0x08) == 0x08) { /* Bit 3 must be 1 */
                mouse_bytes[0] = data;
                mouse_cycle = 1;
            }
            break;
        case 1:
            mouse_bytes[1] = data;
            mouse_cycle = 2;
            break;
        case 2:
            mouse_bytes[2] = data;
            mouse_cycle = 0;

            /* Parse packet */
            uint8_t flags = mouse_bytes[0];
            int dx = (int)mouse_bytes[1];
            int dy = (int)mouse_bytes[2];

            /* Sign extension */
            if (flags & 0x10) dx |= 0xFFFFFF00;
            if (flags & 0x20) dy |= 0xFFFFFF00;

            current_state.left_btn   = (flags & 0x01) != 0;
            current_state.right_btn  = (flags & 0x02) != 0;
            current_state.middle_btn = (flags & 0x04) != 0;

            current_state.x += dx;
            current_state.y -= dy; /* PS/2 Y axis is inverted */

            /* Clamp */
            if (current_state.x < 0) current_state.x = 0;
            if (current_state.y < 0) current_state.y = 0;
            if (current_state.x >= bound_max_x) current_state.x = bound_max_x - 1;
            if (current_state.y >= bound_max_y) current_state.y = bound_max_y - 1;
            break;
    }
}

void mouse_get_state(struct mouse_state *out) {
    if (out) *out = current_state;
}

void mouse_set_bounds(int max_x, int max_y) {
    bound_max_x = max_x;
    bound_max_y = max_y;
}
