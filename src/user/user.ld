/* VibeCagOS user program link script (ELF32, i386).
 *
 * This ELF is a BUILD intermediate only: tools/vbinpack converts it to VBIN,
 * which is what the kernel loads (docs/VBIN.md). Layout rules the packer
 * relies on:
 *
 *   .text + .rodata   contiguous from USER_BASE (0x10000000), read-only
 *   .data + .bss       page aligned after that, read-write
 *
 * Nothing dynamic, nothing shared: no PT_INTERP, no PT_DYNAMIC, no relocations
 * at run time. vbinpack refuses ET_DYN outright.
 */
ENTRY(_start)
SECTIONS {
    . = 0x10000000;

    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(0x1000);
    .data : { *(.data .data.*) }
    .bss  : { *(.bss .bss.* COMMON) }

    /DISCARD/ : {
        *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash)
        *(.gnu.hash) *(.comment) *(.note*) *(.eh_frame*) *(.note.GNU-stack)
    }
}