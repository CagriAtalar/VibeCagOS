/*
 * ps - list processes. A pure user program: it reads the procfs table at
 * /proc/tasks through open()/read() like any other file. The kernel no longer
 * owns a "ps" command, so process inspection exercises procfs + the syscall
 * boundary end to end (docs/USERSPACE.md).
 */
#include "ulib.h"

int user_main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fd = sys_open("/proc/tasks", O_RDONLY, 0);
    if (fd < 0) return ufail("ps", "/proc/tasks", fd);
    char b[256];
    int n;
    int rc = 0;
    while ((n = sys_read(fd, b, sizeof(b))) > 0)
        if (sys_write(1, b, (u32)n) < 0) { rc = 1; break; }
    sys_close(fd);
    return rc;
}
