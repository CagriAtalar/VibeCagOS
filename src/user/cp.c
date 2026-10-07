/* cp - copy a file. Reads the source and writes a fresh destination. */
#include "ulib.h"

static int copy_fd(int in, int out) {
    char b[512];
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
    if (argc != 3) { uputs(2, "usage: cp <src> <dst>\n"); return 1; }
    int in = sys_open(argv[1], O_RDONLY, 0);
    if (in < 0) return ufail("cp", argv[1], in);
    int o = sys_open(argv[2], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (o < 0) { sys_close(in); return ufail("cp", argv[2], o); }
    int r = copy_fd(in, o);
    sys_close(in);
    sys_close(o);
    return r < 0 ? ufail("cp", argv[1], r) : 0;
}
