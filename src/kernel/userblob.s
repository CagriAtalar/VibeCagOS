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
EMBED init
EMBED utest
EMBED ls
EMBED cat
EMBED echo
EMBED pwd
EMBED head
EMBED hexdump
EMBED stat
EMBED mkdir
EMBED rmdir
EMBED rm
EMBED mv
EMBED cp
EMBED touch
EMBED write
EMBED clear
EMBED sleep
EMBED kill
EMBED ps
EMBED uname
EMBED uptime