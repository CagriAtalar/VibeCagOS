/* touch - create empty files (or update them in place). */
#include "ulib.h"

int user_main(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: touch <file...>\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_CREAT | O_WRONLY, 0644);
        if (fd < 0) rc |= ufail("touch", argv[i], fd);
        else sys_close(fd);
    }
    return rc;
}
