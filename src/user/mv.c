/* mv - rename a file or directory (SYS_RENAME). */
#include "ulib.h"

int user_main(int argc, char **argv) {
    if (argc != 3) { uputs(2, "usage: mv <old> <new>\n"); return 1; }
    int r = sys_rename(argv[1], argv[2]);
    return r < 0 ? ufail("mv", argv[1], r) : 0;
}
