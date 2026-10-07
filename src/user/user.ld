/* VibeCagOS user program link script (ELF32, i386).
 *
 * Produces a real ET_EXEC ELF32 image: one PT_LOAD per permission class, so
 * the kernel's ELF loader (src/kernel/elf.c) can map code read-only and data
 * read-write without any special-casing.
 *
 *   .text   R+X   at USER_BASE (0x10000000)
 *   .rodata R     page aligned -> separate PT_LOAD, still read-only
 *   .data   RW    page aligned -> separate PT_LOAD, writable
 *   .bss    RW    NOBITS: p_filesz < p_memsz, the loader leaves it zero
 *
 * Nothing dynamic, nothing shared: no PT_INTERP, no PT_DYNAMIC, no relocations
 * at run time. That is what lets elf.c refuse ET_DYN outright.
 */
ENTRY(_start)
SECTIONS {
    . = 0x10000000;

    .text : { *(.text .text.*) }

    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(0x1000);
    .data : { *(.data .data.*) }
    .bss  : { *(.bss .bss.* COMMON) }

    /DISCARD/ : {
        *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash)
        *(.gnu.hash) *(.comment) *(.note*) *(.eh_frame*) *(.note.GNU-stack)
    }
}