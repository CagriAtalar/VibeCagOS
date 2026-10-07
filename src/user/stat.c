/* stat - show metadata for a path (SYS_STAT). */
#include "ulib.h"

int user_main(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: stat <path>\n"); return 1; }
    struct vibe_stat st;
    int r = sys_stat(argv[1], &st);
    if (r < 0) return ufail("stat", argv[1], r);
    uprintf(1, "  File: %s\n  Type: %s  Size: %u  Mode: %03o  Links: %u  Inode: %u\n",
            argv[1],
            st.type == VIBE_TYPE_DIR ? "directory" :
            st.type == VIBE_TYPE_CHR ? "chardev" : "regular file",
            st.size, st.mode, st.nlink, st.ino);
    return 0;
}
