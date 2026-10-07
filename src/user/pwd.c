/*
 * pwd - print the calling process's current working directory.
 *
 * An ordinary user program (see docs/USERSPACE.md): it asks the kernel for its
 * own cwd via SYS_GETCWD. Because cwd is per-process, two shells in different
 * directories each see their own. No kernel header, no direct hardware access.
 */
#include "ulib.h"

int user_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char b[256];
    int r = sys_getcwd(b, sizeof(b));
    if (r < 0) return ufail("pwd", 0, r);
    uprintf(1, "%s\n", b);
    return 0;
}
