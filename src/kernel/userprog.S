/*
 * VibeCagOS — built-in user programs.
 *
 * Each flat binary produced by the user/ build is embedded into the kernel
 * image with .incbin until a real ELF loader exists. The shell's `run`
 * command copies one of these into a fresh ring 3 address space.
 *
 * Paths are relative to the repository root (where `make` runs).
 */
.section .rodata
.align 16

.macro UPROG name
.global user_\name\()_start
.global user_\name\()_end
user_\name\()_start:
    .incbin "user/\name\().bin"
user_\name\()_end:
    .align 16
.endm

UPROG hello
UPROG spin
UPROG crash
UPROG segv
UPROG upper
