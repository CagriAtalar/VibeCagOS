/*
 * init - VibeCagOS user-space init.
 *
 * The first user process after the kernel's kinit thread. Its only job is to
 * start the shell, wait for it, and propagate its exit status back to kinit
 * (which powers the machine off). It includes no kernel header and uses only
 * syscalls, exactly like any other user program.
 *
 * Single-shot by design: when the shell exits with 0 the user asked to halt,
 * so init exits too and the machine powers off. Restart-on-crash (respawning
 * the shell after a non-zero exit) is future work; see docs/ROADMAP.md.
 */
#include "ulib.h"

int user_main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Filesystem first, embedded fallback: /bin/sh is installed at boot from
     * the embedded image, so both names reach the same program. The path form
     * exercises the VFS-backed loader; the bare name is the safety net. */
    const char *av[] = { "sh", 0 };
    int pid = sys_spawn("/bin/sh", av);
    if (pid < 0)
        pid = sys_spawn("sh", av);
    if (pid < 0) {
        ufail("init", "/bin/sh", pid);
        return 1;
    }

    int st = 0;
    sys_waitpid(pid, &st);
    return st;
}