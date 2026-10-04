/* Minimal user-side syscall layer. No kernel headers, no libc. */
#pragma once
#include "../abi/syscall.h"

typedef unsigned int u32;

static inline int syscall3(int n, u32 a, u32 b, u32 c) {
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory", "cc");
    return r;
}
static inline int sys_write(int fd, const void *b, u32 n) { return syscall3(SYS_WRITE, (u32)fd, (u32)b, n); }
static inline int sys_read(int fd, void *b, u32 n)        { return syscall3(SYS_READ, (u32)fd, (u32)b, n); }
static inline void sys_exit(int s)                         { syscall3(SYS_EXIT, (u32)s, 0, 0); for (;;); }
static inline int sys_getpid(void)                         { return syscall3(SYS_GETPID, 0, 0, 0); }
static inline int sys_sleep(u32 ms)                        { return syscall3(SYS_SLEEP, ms, 0, 0); }
static inline void sys_yield(void)                         { syscall3(SYS_YIELD, 0, 0, 0); }
static inline int sys_uptime(void)                         { return syscall3(SYS_UPTIME, 0, 0, 0); }
static inline int sys_putchar(int c)                       { return syscall3(SYS_PUTCHAR, (u32)c, 0, 0); }
static inline int sys_getinfo(void *p)                     { return syscall3(SYS_GETINFO, (u32)p, 0, 0); }
static inline int sys_open(const char *p, int fl, int mode) { return syscall3(SYS_OPEN, (u32)p, (u32)fl, (u32)mode); }
static inline int sys_close(int fd)                          { return syscall3(SYS_CLOSE, (u32)fd, 0, 0); }
static inline int sys_stat(const char *p, struct vibe_stat *st) { return syscall3(SYS_STAT, (u32)p, (u32)st, 0); }
static inline int sys_fstat(int fd, struct vibe_stat *st)    { return syscall3(SYS_FSTAT, (u32)fd, (u32)st, 0); }
static inline int sys_mkdir(const char *p, int mode)         { return syscall3(SYS_MKDIR, (u32)p, (u32)mode, 0); }
static inline int sys_unlink(const char *p)                  { return syscall3(SYS_UNLINK, (u32)p, 0, 0); }
static inline int sys_rmdir(const char *p)                   { return syscall3(SYS_RMDIR, (u32)p, 0, 0); }
static inline int sys_lseek(int fd, int off, int wh)         { return syscall3(SYS_LSEEK, (u32)fd, (u32)off, (u32)wh); }
static inline int sys_readdir(int fd, struct vibe_dirent *d, u32 max) { return syscall3(SYS_READDIR, (u32)fd, (u32)d, max); }
