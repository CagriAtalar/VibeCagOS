/* hexdump - dump a file as 16 bytes per line with an ASCII gutter. */
#include "ulib.h"

int user_main(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: hexdump <file>\n"); return 1; }
    int fd = sys_open(argv[1], O_RDONLY, 0);
    if (fd < 0) return ufail("hexdump", argv[1], fd);
    unsigned char b[16];
    int n;
    u32 off = 0;
    while ((n = sys_read(fd, b, sizeof(b))) > 0) {
        uprintf(1, "%08x  ", off);
        for (int i = 0; i < 16; i++)
            if (i < n) uprintf(1, "%02x ", b[i]); else uputs(1, "   ");
        uputs(1, " |");
        for (int i = 0; i < n; i++)
            uprintf(1, "%c", b[i] >= 32 && b[i] < 127 ? b[i] : '.');
        uputs(1, "|\n");
        off += (u32)n;
    }
    sys_close(fd);
    return 0;
}
