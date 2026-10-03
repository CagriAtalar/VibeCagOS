/* hello — exercises the basic syscalls and the kernel's pointer validation. */
#include "ulib.h"

static char buf[64];          /* lives in .bss: tests the .pad trick */

int main(void) {
    uputs("Hello from Ring 3! pid=");
    uputint(sys_getpid());
    uputs("\n");

    /* 1. A kernel address must be REJECTED, not obeyed. */
    int r = sys_readfile("/etc/version", (void *)0x00100000, 16);
    uputs("read into kernel memory -> ");
    uputint(r);
    uputs(r == -1 ? "  (rejected, good)\n" : "  (BUG: accepted!)\n");

    /* 2. A NULL path must be rejected too. */
    r = sys_readfile((const char *)0, buf, sizeof buf);
    uputs("read with NULL path     -> ");
    uputint(r);
    uputs(r == -1 ? "  (rejected, good)\n" : "  (BUG: accepted!)\n");

    /* 3. File round trip through the VFS using only valid pointers. */
    const char msg[] = "written from user space";
    r = sys_writefile("/tmp/user.txt", msg, ustrlen(msg));
    uputs("write /tmp/user.txt     -> ");
    uputint(r);
    uputs("\n");
    r = sys_readfile("/tmp/user.txt", buf, sizeof buf - 1);
    if (r >= 0) buf[r] = '\0';
    uputs("read  /tmp/user.txt     -> ");
    uputs(r >= 0 ? buf : "(failed)");
    uputs("\n");

    /* 4. Sleep and measure it with the kernel's tick counter. */
    int t0 = sys_uptime_ms();
    sys_sleep_ms(200);
    uputs("slept ~");
    uputint(sys_uptime_ms() - t0);
    uputs(" ms (asked for 200)\n");

    return 42;
}
