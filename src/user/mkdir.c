/*
 * mkdir - create one or more directories. Talks only to the kernel through
 * SYS_MKDIR; reports errors in the shared "cmd: what: reason" format.
 */
#include "ulib.h"

int user_main(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: mkdir <dir...>\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = sys_mkdir(argv[i], 0755);
        if (r < 0) rc |= ufail("mkdir", argv[i], r);
    }
    return rc;
}
