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
PROG(init)
PROG(utest)
PROG(ls)
PROG(cat)
PROG(echo)
PROG(pwd)
PROG(head)
PROG(hexdump)
PROG(stat)
PROG(mkdir)
PROG(rmdir)
PROG(rm)
PROG(mv)
PROG(cp)
PROG(touch)
PROG(write)
PROG(clear)
PROG(sleep)
PROG(kill)
PROG(ps)
PROG(uname)
PROG(uptime)

const struct user_prog user_progs[] = {
    { "sh",      prog_sh_start,      prog_sh_end      },
    { "init",    prog_init_start,    prog_init_end    },
    { "utest",   prog_utest_start,   prog_utest_end   },
    { "ls",      prog_ls_start,      prog_ls_end      },
    { "cat",     prog_cat_start,     prog_cat_end     },
    { "echo",    prog_echo_start,    prog_echo_end    },
    { "pwd",     prog_pwd_start,     prog_pwd_end     },
    { "head",    prog_head_start,    prog_head_end    },
    { "hexdump", prog_hexdump_start, prog_hexdump_end },
    { "stat",    prog_stat_start,    prog_stat_end    },
    { "mkdir",   prog_mkdir_start,   prog_mkdir_end   },
    { "rmdir",   prog_rmdir_start,   prog_rmdir_end   },
    { "rm",      prog_rm_start,      prog_rm_end      },
    { "mv",      prog_mv_start,      prog_mv_end      },
    { "cp",      prog_cp_start,      prog_cp_end      },
    { "touch",   prog_touch_start,   prog_touch_end   },
    { "write",   prog_write_start,   prog_write_end   },
    { "clear",   prog_clear_start,   prog_clear_end   },
    { "sleep",   prog_sleep_start,   prog_sleep_end   },
    { "kill",    prog_kill_start,    prog_kill_end    },
    { "ps",      prog_ps_start,      prog_ps_end      },
    { "uname",   prog_uname_start,   prog_uname_end   },
    { "uptime",  prog_uptime_start,  prog_uptime_end  },
};
const int user_prog_count = sizeof(user_progs) / sizeof(user_progs[0]);

const struct user_prog *user_prog_find(const char *name) {
    for (int i = 0; i < user_prog_count; i++)
        if (strcmp(user_progs[i].name, name) == 0) return &user_progs[i];
    return NULL;
}
