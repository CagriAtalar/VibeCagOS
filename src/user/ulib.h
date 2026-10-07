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
static inline int sys_spawn(const char *name, const char *const *argv) { return syscall3(SYS_SPAWN, (u32)name, (u32)argv, 0); }
static inline int sys_waitpid(int pid, int *status)          { return syscall3(SYS_WAITPID, (u32)pid, (u32)status, 0); }
static inline int sys_chdir(const char *p)                   { return syscall3(SYS_CHDIR, (u32)p, 0, 0); }
static inline int sys_getcwd(char *b, u32 n)                 { return syscall3(SYS_GETCWD, (u32)b, n, 0); }
static inline int sys_rename(const char *a, const char *b)   { return syscall3(SYS_RENAME, (u32)a, (u32)b, 0); }
static inline int sys_kill(int pid)                          { return syscall3(SYS_KILL, (u32)pid, 0, 0); }
static inline int sys_clear(void)                            { return syscall3(SYS_CLEAR, 0, 0, 0); }
/* Create a pipe; returns 0 and fills fds[0]=read end, fds[1]=write end. */
static inline int sys_pipe(int *fds)                         { return syscall3(SYS_PIPE, (u32)fds, 0, 0); }
/* Replace this process with the ELF at `path`. Returns only on failure. */
static inline int sys_exec(const char *path, const char *const *argv) {
    return syscall3(SYS_EXEC, (u32)path, (u32)argv, 0);
}
/* Spawn a child whose fds 0/1/2 are this process's in/out/err descriptors;
 * a negative value leaves that fd closed in the child. */
static inline int sys_spawnfds(const char *name, const char *const *argv,
                               int in, int out, int err) {
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(SYS_SPAWNFDS), "b"((u32)name), "c"((u32)argv),
                           "d"((u32)in), "S"((u32)out), "D"((u32)err)
                         : "memory", "cc");
    return r;
}

/* ---- tiny libc (ulib.c). No kernel code is reachable from here. ---- */
typedef unsigned int usize;
usize  strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, usize n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, usize n);
void  *memcpy(void *d, const void *s, usize n);
void  *memmove(void *d, const void *s, usize n);
void  *memset(void *d, int c, usize n);
int    memcmp(const void *a, const void *b, usize n);
int    atoi(const char *s);
/* printf to a file descriptor: %s %d %u %x %c %% with optional '-', '0', width */
int    uprintf(int fd, const char *fmt, ...);
int    uputs(int fd, const char *s);
/* Read a line from fd 0 with echo + backspace. Returns length, or -1 on error. */
int    ugetline(char *buf, int max);
