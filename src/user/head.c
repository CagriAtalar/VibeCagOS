/*
 * head - print the first N (default 10) lines of each file, or of standard
 * input when no file is given. Reading fd 0 with no argument is what lets it
 * compose in a pipeline:  ls /bin | head
 */
#include "ulib.h"

static int head_fd(int fd, int lines) {
    char b[128];
    int seen = 0, n;
    while (seen < lines && (n = sys_read(fd, b, sizeof(b))) > 0) {
        int k = 0;
        for (; k < n && seen < lines; k++) if (b[k] == '\n') seen++;
        if (sys_write(1, b, (u32)k) < 0) return -E_IO;
    }
    return n < 0 ? n : 0;
}

int user_main(int argc, char **argv) {
    int lines = 10, i = 1;
    if (argc > 2 && !strcmp(argv[1], "-n")) { lines = atoi(argv[2]); i = 3; }
    if (i >= argc) return head_fd(0, lines) < 0 ? 1 : 0;

    int rc = 0;
    for (; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY, 0);
        if (fd < 0) { rc |= ufail("head", argv[i], fd); continue; }
        int r = head_fd(fd, lines);
        sys_close(fd);
        if (r < 0) rc |= ufail("head", argv[i], r);
    }
    return rc;
}
