/* write - create a file containing its arguments, space separated. */
#include "ulib.h"

int user_main(int argc, char **argv) {
    if (argc < 3) { uputs(2, "usage: write <file> <text...>\n"); return 1; }
    int fd = sys_open(argv[1], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return ufail("write", argv[1], fd);
    for (int i = 2; i < argc; i++) {
        sys_write(fd, argv[i], (u32)strlen(argv[i]));
        sys_write(fd, i + 1 < argc ? " " : "\n", 1);
    }
    sys_close(fd);
    return 0;
}
