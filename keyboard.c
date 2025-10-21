#include "common.h"
#include "kernel.h"

// PS/2 Keyboard ports
#define KB_DATA_PORT    0x60
#define KB_STATUS_PORT  0x64

// Keyboard buffer
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static volatile int kb_read_pos = 0;
static volatile int kb_write_pos = 0;

// US QWERTY scancode to ASCII mapping
static char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};

static char scancode_to_ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

static bool shift_pressed = false;

// Check if keyboard has data
static inline bool kb_has_data(void) {
    return (inb(KB_STATUS_PORT) & 0x01) != 0;
}

// Read scancode from keyboard
static inline uint8_t kb_read_scancode(void) {
    while (!kb_has_data());
    return inb(KB_DATA_PORT);
}

// Handle keyboard scancode
void keyboard_handler(void) {
    uint8_t scancode = inb(KB_DATA_PORT);
    
    // Handle key release (scancode & 0x80)
    if (scancode & 0x80) {
        scancode &= 0x7F;
        // Check for shift release
        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = false;
        }
        return;
    }
    
    // Check for shift press
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = true;
        return;
    }
    
    // Convert scancode to ASCII
    char ascii = 0;
    if (scancode < 128) {
        ascii = shift_pressed ? scancode_to_ascii_shift[scancode] : scancode_to_ascii[scancode];
    }
    
    if (ascii != 0) {
        // Add to buffer
        int next_pos = (kb_write_pos + 1) % KB_BUFFER_SIZE;
        if (next_pos != kb_read_pos) {
            kb_buffer[kb_write_pos] = ascii;
            kb_write_pos = next_pos;
        }
    }
}

// Get character from keyboard buffer
long kb_getchar(void) {
    if (kb_read_pos == kb_write_pos) {
        return -1;  // No data
    }
    
    char ch = kb_buffer[kb_read_pos];
    kb_read_pos = (kb_read_pos + 1) % KB_BUFFER_SIZE;
    return ch;
}

// Initialize keyboard
void keyboard_init(void) {
    kb_read_pos = 0;
    kb_write_pos = 0;
    shift_pressed = false;
    
    // Clear keyboard buffer
    while (kb_has_data()) {
        inb(KB_DATA_PORT);
    }
}

