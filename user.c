#include "user.h"

extern char __stack_top[];

int syscall(int sysno, int arg0, int arg1, int arg2) {
    int ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=b"(ret)
        : "a"(sysno), "b"(arg0), "c"(arg1), "d"(arg2)
        : "memory"
    );
    return ret;
}

int ls(void) {
    return syscall(SYS_LS, 0, 0, 0);
}

void putchar(char ch) {
    syscall(SYS_PUTCHAR, ch, 0, 0);
}

int getchar(void) {
    return syscall(SYS_GETCHAR, 0, 0, 0);
}

int readfile(const char *filename, char *buf, int len) {
    return syscall(SYS_READFILE, (int) filename, (int) buf, len);
}

int writefile(const char *filename, const char *buf, int len) {
    return syscall(SYS_WRITEFILE, (int) filename, (int) buf, len);
}

int addfile(const char *filename) {
    return syscall(SYS_ADDFILE, (int) filename, 0, 0);
}

int readf(const char *filename) {
    return syscall(SYS_READF, (int)filename, 0, 0);
}

int addf(const char *filename) {
    return syscall(SYS_ADDF, (int)filename, 0, 0);
}

int writef(const char *filename, const char *buf, int len) {
    return syscall(SYS_WRITEF, (int)filename, (int)buf, len);
}

__attribute__((noreturn)) void exit(void) {
    syscall(SYS_EXIT, 0, 0, 0);
    for (;;);
}

__attribute__((section(".text.start")))
__attribute__((naked))
void start(void) {
    __asm__ __volatile__(
        "mov %[stack_top], %%esp\n"
        "call main\n"
        "call exit\n"
        :
        : [stack_top] "r"(__stack_top)
    );
}

