/* uptime - print time since boot, read from /proc/uptime. */
#include "ulib.h"

int user_main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fd = sys_open("/proc/uptime", O_RDONLY, 0);
    if (fd < 0) return ufail("uptime", "/proc/uptime", fd);
    char b[128];
    int n;
    while ((n = sys_read(fd, b, sizeof(b))) > 0)
        if (sys_write(1, b, (u32)n) < 0) return 1;
    sys_close(fd);
    return 0;
}
