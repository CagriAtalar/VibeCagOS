/* uname - print the OS identity line, read from /proc/version. */
#include "ulib.h"

int user_main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fd = sys_open("/proc/version", O_RDONLY, 0);
    if (fd < 0) return ufail("uname", "/proc/version", fd);
    char b[256];
    int n;
    while ((n = sys_read(fd, b, sizeof(b))) > 0)
        if (sys_write(1, b, (u32)n) < 0) return 1;
    sys_close(fd);
    return 0;
}
