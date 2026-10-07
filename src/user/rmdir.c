/* rmdir - remove empty directories (SYS_RMDIR). */
#include "ulib.h"

int user_main(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: rmdir <dir...>\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = sys_rmdir(argv[i]);
        if (r < 0) rc |= ufail("rmdir", argv[i], r);
    }
    return rc;
}
