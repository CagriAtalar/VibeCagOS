/*
 * ls - list directory contents. An ordinary user program: it lives in /bin,
 * is loaded by the kernel's VBIN loader and talks to the kernel only through
 * int 0x80. It includes no kernel header.
 *
 *   ls [-l] [dir]
 */
#include "ulib.h"

int user_main(int argc, char **argv) {
    int longfmt = 0;
    const char *path = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) longfmt = 1;
        else path = argv[i];
    }

    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return ufail("ls", path, fd);

    struct vibe_dirent d;
    int rc = 0, r;
    while ((r = sys_readdir(fd, &d, 1)) == 1) {
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
        char t = d.type == VIBE_TYPE_DIR ? 'd' : d.type == VIBE_TYPE_CHR ? 'c' : '-';
        if (!longfmt) { uprintf(1, "%c  %s\n", t, d.name); continue; }

        char full[300];
        struct vibe_stat st;
        u32 n = (u32)strlen(path);
        strncpy(full, path, sizeof(full) - VIBE_NAME_MAX - 2);
        full[n] = 0;
        if (n && full[n - 1] != '/') strcpy(full + n, "/");
        strncpy(full + strlen(full), d.name, VIBE_NAME_MAX);
        if (sys_stat(full, &st) < 0) { st.size = 0; st.mode = 0; }
        uprintf(1, "%c %03o %6u  %s\n", t, st.mode, st.size, d.name);
    }
    if (r < 0) rc = ufail("ls", path, r);
    sys_close(fd);
    return rc;
}