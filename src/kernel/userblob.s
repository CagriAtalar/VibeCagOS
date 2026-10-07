/* Embeds the separately built user ELF images into the kernel image.
 *
 * Phase 1 of the migration: programs are linked here instead of being read
 * from the filesystem, so a fresh build boots without needing a populated
 * disk image. The bytes are ordinary ELF32 executables and go through exactly
 * the same loader (src/kernel/elf.c) as a filesystem-backed program would.
 *
 * To add a program: add it to USER_PROGS in the Makefile, a PROG() line and a
 * table entry in progs.c, and one EMBED line here. */
.section .rodata
.balign 4
.macro EMBED name
.global prog_\name\()_start
.global prog_\name\()_end
prog_\name\()_start:
    .incbin "\name\().elf"
prog_\name\()_end:
.balign 4
.endm

EMBED sh
EMBED ls
EMBED cat
EMBED echo
EMBED utest