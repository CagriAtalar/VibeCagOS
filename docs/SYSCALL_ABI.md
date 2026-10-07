# VibeCagOS — Syscall ABI

> The one source of truth is `src/abi/syscall.h`, shared verbatim by kernel
> and userland. This document summarizes it; if they disagree, the header
> wins and this file must be fixed.

## Mechanism

`int 0x80`. Only interrupt vector reachable from Ring 3 (IDT DPL 3; all
hardware vectors are DPL 0 and fault with `#GP` if user code invokes them).

```
EAX = syscall number
EBX = arg1, ECX = arg2, EDX = arg3, ESI = arg4, EDI = arg5
return in EAX; negative = -errno
```

## Numbers

| # | Name | Args | Notes |
|---|---|---|---|
| 1 | EXIT | status | never returns |
| 2 | WRITE | fd, buf, len | reads user buf |
| 3 | READ | fd, buf, len | BLOCKS until ≥1 byte; writes user buf |
| 4 | GETPID | — | |
| 5 | SLEEP | ms | BLOCKS |
| 6 | YIELD | — | reschedules |
| 7 | UPTIME | — | ms since boot |
| 8 | PUTCHAR | c | console |
| 9 | GETINFO | vibe_info* | writes user buf |
| 10 | OPEN | path, flags, mode | → fd; reads path |
| 11 | CLOSE | fd | |
| 12 | STAT | path, vibe_stat* | path + writes buf |
| 13 | MKDIR | path, mode | |
| 14 | UNLINK | path | |
| 15 | LSEEK | fd, off, whence | → new offset |
| 16 | READDIR | fd, vibe_dirent*, max | → count (0 = end) |
| 17 | RMDIR | path | |
| 18 | FSTAT | fd, vibe_stat* | writes buf |
| 19 | SPAWN | name, argv | → pid; bare names try `/bin` first |
| 20 | WAITPID | pid\|-1, status* | BLOCKS; reaps; `-ECHILD` if none |
| 21 | CHDIR | path | per-process cwd |
| 22 | GETCWD | buf, size | → len |
| 23 | RENAME | old, new | |
| 24 | KILL | pid | immediate terminate |
| 25 | CLEAR | — | clear console |
| 26 | PIPE | int fds[2] | → 0; fds[0]=read, fds[1]=write |
| 27 | EXEC | path, argv | replaces address space; returns only on failure |
| 28 | SPAWNFDS | name, argv, in, out, err | like SPAWN; child fds 0/1/2 from caller's descriptors (`-1` = closed) |
| 29 | TRUNCATE | path, size | set file size, freeing the blocks dropped from the end |

## File descriptors

Per-process table. `0` stdin (console, read-only, blocking), `1`/`2`
stdout/stderr (console, write-only). New fds are the lowest free index ≥ 3,
`OPEN_MAX` 16 (`-EMFILE` when full). `O_RDONLY` write and `O_WRONLY` read
fail with `-EBADF`.

## Errors

Negative `-E_*` (`E_NOENT 2`, `E_SRCH 3`, `E_NOEXEC 8`, `E_BADF 9`,
`E_CHILD 10`, `E_FAULT 14`, `E_EXIST 17`, `E_NOTDIR 20`, `E_ISDIR 21`,
`E_INVAL 22`, `E_MFILE 24`, `E_NOSPC 28`, `E_PIPE 32`, `E_NAMETOOLONG 36`,
`E_NOSYS 38`, `E_NOTEMPTY 39`, …). VFS errors are translated in `sysfile.c`.

## User-memory rules

Every pointer argument is untrusted. The kernel touches it only through
`copy_from_user` / `copy_to_user` / `strncpy_from_user` /
`user_range_valid`, which require, for every page: inside
`[USER_BASE, USER_END)`, no wrap, PDE+PTE present with `USER` (and `WRITABLE`
for destinations). Nothing is copied unless the whole range validates.
Violations return `-EFAULT`; a faulting user access kills only that process.
