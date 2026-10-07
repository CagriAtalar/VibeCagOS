/*
 * VibeCagOS - syscall ABI (shared by kernel and user programs).
 * Pure #defines: must not include any kernel header.
 *
 *   int 0x80      EAX = number, EBX/ECX/EDX/ESI/EDI = arg1..arg5
 *   return        EAX (negative = -errno)
 *
 * Per-syscall contract (blocks? / touches user memory?):
 *   SYS_EXIT    (status)            never returns          no memory
 *   SYS_WRITE   (fd, buf, len)      may not block          reads user buf
 *   SYS_READ    (fd, buf, len)      BLOCKS until >=1 byte  writes user buf
 *   SYS_GETPID  ()                  -                      -
 *   SYS_SLEEP   (ms)                BLOCKS                 -
 *   SYS_YIELD   ()                  reschedules            -
 *   SYS_UPTIME  ()                  returns ms             -
 *   SYS_PUTCHAR (c)                 -                      -
 *   SYS_GETINFO (struct vibe_info*) -                      writes user buf
 *   SYS_OPEN    (path, flags, mode) -> fd   fs I/O, no sleep   reads path
 *   SYS_CLOSE   (fd)
 *   SYS_STAT    (path, vibe_stat*)  fs I/O                     path + writes buf
 *   SYS_FSTAT   (fd, vibe_stat*)                               writes buf
 *   SYS_MKDIR   (path, mode)        fs I/O                     reads path
 *   SYS_UNLINK  (path)              fs I/O                     reads path
 *   SYS_RMDIR   (path)              fs I/O                     reads path
 *   SYS_LSEEK   (fd, off, whence) -> new offset
 *   SYS_READDIR (fd, vibe_dirent*, max) -> count (0 = end)     writes buf
 *
 *   SYS_SPAWN   (name, argv) -> pid  create child from a built-in program;
 *                                    argv = NULL-terminated array of strings (<= SPAWN_ARGS_MAX)
 *   SYS_SPAWNFDS(name, argv, in, out, err) -> pid
 *                                    as SYS_SPAWN, but the child's fds 0/1/2 come
 *                                    from the caller's descriptors in/out/err;
 *                                    a negative value leaves that fd closed in
 *                                    the child. Every other child fd is closed.
 *                                    This is how a shell does `>` and `|` without
 *                                    fork()+dup2().
 *   SYS_WAITPID (pid|-1, int *status) BLOCKS until a child exits; reaps it; -ECHILD if none
 *   SYS_CHDIR   (path)              per-process working directory
 *   SYS_GETCWD  (buf, size) -> len
 *   SYS_RENAME  (old, new)
 *   SYS_KILL    (pid)               terminates another process
 *   SYS_CLEAR   ()                  clear the console
 *   SYS_PIPE    (int fds[2]) -> 0    create a pipe; fds[0]=read end, fds[1]=write
 *                                 end, allocated as the two lowest free fds.
 *                                 Writes user buf into the pipe
 *   SYS_EXEC    (path, argv)         replace THIS process's address space with
 *                                 the ELF at `path`; fds 0/1/2 are kept, every
 *                                 other fd is closed. Returns only on failure
 *                                 (-ENOENT / -EACCES / -ENOEXEC / -ENOMEM ...);
 *                                 on success it does not return to Ring 3 at
 *                                 all: the process starts running the new image.
 *
 * Program images are ELF32 executables (static, ET_EXEC, PT_LOAD only). A
 * `path` containing '/' is read through the VFS; a bare name is looked up in
 * the kernel's built-in program table.
 *
 * File descriptors: per-process table. fd 0 = stdin (console, read only),
 * fd 1 = stdout, fd 2 = stderr (console, write only). New fds are the
 * lowest free index >= 3.
 */
#ifndef VIBE_ABI_SYSCALL_H
#define VIBE_ABI_SYSCALL_H

#define SYS_EXIT     1
#define SYS_WRITE    2
#define SYS_READ     3
#define SYS_GETPID   4
#define SYS_SLEEP    5
#define SYS_YIELD    6
#define SYS_UPTIME   7
#define SYS_PUTCHAR  8
#define SYS_GETINFO  9
#define SYS_OPEN    10
#define SYS_CLOSE   11
#define SYS_STAT    12
#define SYS_MKDIR   13
#define SYS_UNLINK  14
#define SYS_LSEEK   15
#define SYS_READDIR 16
#define SYS_RMDIR   17
#define SYS_FSTAT   18
#define SYS_SPAWN   19
#define SYS_WAITPID 20
#define SYS_CHDIR   21
#define SYS_GETCWD  22
#define SYS_RENAME  23
#define SYS_KILL    24
#define SYS_CLEAR   25
#define SYS_PIPE    26
#define SYS_EXEC    27
#define SYS_SPAWNFDS 28
#define SYS_MAX     28

#define E_PERM    1
#define E_SRCH    3
#define E_CHILD  10
#define E_NOENT   2
#define E_IO      5
#define E_BADF    9
#define E_NOMEM  12
#define E_ACCES  13
#define E_FAULT  14
#define E_EXIST  17
#define E_NOTDIR 20
#define E_ISDIR  21
#define E_INVAL  22
#define E_MFILE  24
#define E_NOSPC  28
#define E_NAMETOOLONG 36
#define E_NOSYS  38
#define E_NOTEMPTY 39
#define E_PIPE  32
#define E_NOEXEC 8    /* not an ELF executable, or not executable at all */

/* open() flags (Linux-compatible values) */
#define O_RDONLY   0x000
#define O_WRONLY   0x001
#define O_RDWR     0x002
#define O_CREAT    0x040
#define O_TRUNC    0x200
#define O_APPEND   0x400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define VIBE_TYPE_REG 1
#define VIBE_TYPE_DIR 2
#define VIBE_TYPE_CHR 4

struct vibe_stat {
    unsigned int ino;
    unsigned int type;     /* VIBE_TYPE_* */
    unsigned int mode;
    unsigned int size;
    unsigned int nlink;
};

#define VIBE_NAME_MAX 63
struct vibe_dirent {
    unsigned int ino;
    unsigned int type;
    char name[VIBE_NAME_MAX + 1];   /* truncated, always NUL terminated */
};

struct vibe_info {
    unsigned int pid;
    unsigned int ppid;
    unsigned int ticks;
    unsigned int uptime_ms;
};

/* Written into the user's int[2] by SYS_PIPE. */
#define VIBE_PIPE_R 0
#define VIBE_PIPE_W 1

/* Pipe buffer capacity in bytes. Part of the ABI so that user programs can
 * size their writes; the kernel's struct pipe uses the same value.
 *
 * A pipe is a BLOCKING byte stream: writing more than VIBE_PIPE_SIZE bytes
 * into a pipe with no concurrent reader blocks until a reader drains it, so a
 * single process must never write more than this without reading in between. */
#define VIBE_PIPE_SIZE 4096

/* User program arguments (see process_create_user's System-V style stack). */
#define SPAWN_ARGS_MAX 8
#define SPAWN_ARG_LEN  96

#endif
