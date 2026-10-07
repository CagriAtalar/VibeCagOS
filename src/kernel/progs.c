/*
 * Built-in user programs, embedded in the kernel image (staging only).
 *
 * Each entry is a VBIN executable packed by tools/vbinpack (src/user,
 * Makefile) and loaded by the VBIN loader in vbin.c. This table only says
 * where the bytes live; the loading itself is the same code path a
 * filesystem program uses.
 */
#include "kernel.h"

#define PROG(n) extern const uint8_t prog_##n##_start[], prog_##n##_end[];
PROG(sh)
PROG(ls)
PROG(cat)
PROG(echo)
PROG(utest)
PROG(init)

const struct user_prog user_progs[] = {
    { "sh",    prog_sh_start,    prog_sh_end    },
    { "ls",    prog_ls_start,    prog_ls_end    },
    { "cat",   prog_cat_start,   prog_cat_end   },
    { "echo",  prog_echo_start,  prog_echo_end  },
    { "utest", prog_utest_start, prog_utest_end },
    { "init",  prog_init_start,  prog_init_end  },
};
const int user_prog_count = sizeof(user_progs) / sizeof(user_progs[0]);

const struct user_prog *user_prog_find(const char *name) {
    for (int i = 0; i < user_prog_count; i++)
        if (strcmp(user_progs[i].name, name) == 0) return &user_progs[i];
    return NULL;
}
