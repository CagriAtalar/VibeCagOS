/* Flat user image linked at USER_BASE. Layout (see abi/syscall.h vbin_header):
 *   [header][text][rodata]  -> pages mapped read-only (text_size, page aligned)
 *   [data][bss]             -> pages mapped read-write
 */
ENTRY(_start)
SECTIONS {
    . = 0x10000000;
    __image_start = .;
    .header : {
        LONG(0x4E494256)                     /* VBIN_MAGIC */
        LONG(_start)                         /* entry */
        LONG(__text_end - __image_start)     /* text_size */
        LONG(__data_end - __image_start)     /* file_size */
        LONG(__bss_end  - __image_start)     /* mem_size  */
    }
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    __text_end = .;
    .data   : { *(.data .data.*) }
    __data_end = .;
    .bss    : { *(.bss .bss.* COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    /DISCARD/ : { *(.interp .dynamic .dynsym .dynstr .hash .gnu.hash .comment .note* .eh_frame* .debug*) }
}
