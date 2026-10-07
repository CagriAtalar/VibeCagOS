/*
 * Built-in user programs, embedded in the kernel image (Phase 1 staging).
 *
 * Each entry is a separately built ELF32 executable (src/user, Makefile),
 * loaded by the ELF loader in elf.c. This table only says where the bytes
 * live; the loading itself is the same code path a filesystem program uses,
 * so switching SYS_SPAWN over to the VFS does not touch the loader.
 */
#include "kernel.h"

#define PROG(n) extern const uint8_t prog_##n##_start[], prog_##n##_end[];
PROG(sh)
PROG(utest)

const struct user_prog user_progs[] = {
    { "sh",    prog_sh_start,    prog_sh_end    },
    { "utest", prog_utest_start, prog_utest_end },
};
const int user_prog_count = sizeof(user_progs) / sizeof(user_progs[0]);

const struct user_prog *user_prog_find(const char *name) {
    for (int i = 0; i < user_prog_count; i++)
        if (strcmp(user_progs[i].name, name) == 0) return &user_progs[i];
    return NULL;
}
