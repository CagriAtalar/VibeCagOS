/*
 * Built-in user programs (Phase 1 of the plan: embedded in the kernel image).
 * Each is a separately built flat VBIN binary (see src/user, Makefile).
 * Phase 4 replaces this table with filesystem-backed exec + ELF.
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
