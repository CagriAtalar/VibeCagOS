/* rm - unlink one or more files (SYS_UNLINK). */
#include "ulib.h"

int user_main(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: rm <file...>\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = sys_unlink(argv[i]);
        if (r < 0) rc |= ufail("rm", argv[i], r);
    }
    return rc;
}
