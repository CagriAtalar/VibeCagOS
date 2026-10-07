/* Embeds the separately built user binaries into the kernel image.
 * To add a program: add it to USER_PROGS in the Makefile, a PROG() line and a
 * table entry in progs.c, and one EMBED line here. */
.section .rodata
.balign 4
.macro EMBED name
.global prog_\name\()_start
.global prog_\name\()_end
prog_\name\()_start:
    .incbin "\name\().bin"
prog_\name\()_end:
.balign 4
.endm

EMBED sh
EMBED utest
