/* Embeds the separately built user VBIN images into the kernel image.
 *
 * Staging only: programs are packed here instead of being read from a
 * populated disk image, so a fresh build boots. The bytes are ordinary VBIN
 * executables and go through exactly the same loader (src/kernel/vbin.c) as
 * a filesystem-backed program would.
 *
 * To add a program: add it to USER_PROGS in the Makefile, a PROG() line and a
 * table entry in progs.c, and one EMBED line here. */
.section .rodata
.balign 4
.macro EMBED name
.global prog_\name\()_start
.global prog_\name\()_end
prog_\name\()_start:
    .incbin "\name\().vbin"
prog_\name\()_end:
.balign 4
.endm

EMBED sh
EMBED ls
EMBED cat
EMBED echo
EMBED utest
EMBED init