/*
 * cat - copy files (or stdin) to stdout. Runs in Ring 3 and, because it only
 * ever calls sys_write(), it composes with the shell's pipelines:
 *
 *     utest 25 | cat
 *
 * includes no kernel header.
 */
#include "ulib.h"

static int copy_fd(int in, int out) {
    char b[256];
    int n;
    while ((n = sys_read(in, b, sizeof(b))) > 0) {
        int off = 0;
        while (off < n) {
            int w = sys_write(out, b + off, (u32)(n - off));
            if (w <= 0) return w ? w : -E_IO;
            off += w;
        }
    }
    return n;
}

int user_main(int argc, char **argv) {
    if (argc < 2) return copy_fd(0, 1) < 0 ? 1 : 0;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY, 0);
        if (fd < 0) { rc |= ufail("cat", argv[i], fd); continue; }
        int r = copy_fd(fd, 1);
        sys_close(fd);
        if (r < 0) rc |= ufail("cat", argv[i], r);
    }
    return rc;
}