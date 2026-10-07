/* kill - terminate another process by pid (SYS_KILL). */
#include "ulib.h"

int user_main(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: kill <pid>\n"); return 1; }
    int pid = atoi(argv[1]);
    int r = sys_kill(pid);
    if (r < 0) return ufail("kill", argv[1], r);
    uprintf(1, "kill: terminated pid %d\n", pid);
    return 0;
}
