/*
 * VibeCagOS user program linker script.
 *
 * Programs are flat binaries loaded at USER_BASE (src/kernel/kernel.h).
 * `_start` MUST be the first thing in .text: the kernel jumps to USER_BASE.
 *
 * .pad forces .bss into the on-disk image (objcopy -O binary only emits
 * everything up to the last PROGBITS section), so the kernel's page-by-page
 * copy also covers zero-initialised globals.
 */
ENTRY(_start)

SECTIONS {
    . = 0x40000000;

    .text   : { *(.text._start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.* COMMON) }
    .pad    : { BYTE(0) }

    /DISCARD/ : { *(.interp) *(.comment) *(.note*) *(.eh_frame*) }
}
