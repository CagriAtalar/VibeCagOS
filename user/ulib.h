#pragma once
/*
 * VibeCagOS — minimal user-space runtime ("libc-lite").
 *
 * Syscall ABI (int 0x80): eax = number, ebx/ecx/edx = args, eax = result.
 * Syscall numbers are shared with the kernel through common.h.
 */
#include "common.h"

static inline int syscall3(int nr, int a, int b, int c) {
    int ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(nr), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return ret;
}

static inline void sys_putchar(char c)          { syscall3(SYS_PUTCHAR, c, 0, 0); }
static inline int  sys_getchar(void)            { return syscall3(SYS_GETCHAR, 0, 0, 0); }
static inline void sys_exit(int code)           { syscall3(SYS_EXIT, code, 0, 0); for (;;) {} }
static inline int  sys_getpid(void)             { return syscall3(SYS_GETPID, 0, 0, 0); }
static inline void sys_sleep_ms(int ms)         { syscall3(SYS_SLEEP, ms, 0, 0); }
static inline void sys_yield(void)              { syscall3(SYS_YIELD, 0, 0, 0); }
static inline int  sys_uptime_ms(void)          { return syscall3(SYS_UPTIME, 0, 0, 0); }
static inline int  sys_readfile(const char *p, void *buf, int n)
                                                { return syscall3(SYS_READFILE, (int)p, (int)buf, n); }
static inline int  sys_writefile(const char *p, const void *buf, int n)
                                                { return syscall3(SYS_WRITEFILE, (int)p, (int)buf, n); }

static inline void uputs(const char *s) { while (*s) sys_putchar(*s++); }

static inline void uputint(int v) {
    char tmp[12];
    int i = 0;
    unsigned u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) sys_putchar('-');
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (i--) sys_putchar(tmp[i]);
}

static inline int ustrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Entry point: every program defines main(); _start calls it and exits. */
int main(void);
__attribute__((section(".text._start"), noreturn, used))
void _start(void) { sys_exit(main()); for (;;) {} }
