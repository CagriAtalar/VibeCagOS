# VibeCagOS — User Space

> **Status:** implemented. Every command a user can run, except the five
> operations that must execute in the shell's own process, is a Ring-3 program
> on disk. This document describes what exists; see `docs/ROADMAP.md` for what
> does not.

## The boundary

User programs are ordinary ELF*→*VBIN executables (docs/VBIN.md). They run in
Ring 3 with a private page directory; their only way into the kernel is
`int 0x80` (docs/SYSCALL_ABI.md). They are compiled against `src/user/ulib.h`
and the shared ABI `src/abi/syscall.h`, and the build refuses any `src/user`
file that includes a kernel header (`make check-boundary`).

```
user source (.c)
   |  clang -m32 -ffreestanding
   v
program.elf  (build intermediate)   src/user/user.ld
   |  tools/vbinpack  (host)
   v
program.vbin
   |  Makefile USER_PROGS  ->  .incbin  ->  src/kernel/userblob.s
   v
installed at boot into /bin (and /sbin/init)
   |  sys_spawn / sys_exec
   v
vbin_load() -> Ring 3
```

## Programs in `/bin`

| Program | Source | Kernel surface it uses |
|---|---|---|
| `sh` | `sh.c` | read/write, open/close, spawnfds, waitpid, exec, getcwd, chdir, pipe |
| `utest` | `utest.c` | the whole syscall surface (self-test) |
| `ls` | `ls.c` | open/readdir/stat |
| `cat` | `cat.c` | open/read/write |
| `echo` | `echo.c` | write |
| `pwd` | `pwd.c` | getcwd |
| `head` | `head.c` | open/read (fd 0 when no file) |
| `hexdump` | `hexdump.c` | open/read |
| `stat` | `stat.c` | stat |
| `touch` | `touch.c` | open |
| `write` | `write.c` | open/write |
| `mkdir` | `mkdir.c` | mkdir |
| `rmdir` | `rmdir.c` | rmdir |
| `rm` | `rm.c` | unlink |
| `mv` | `mv.c` | rename |
| `cp` | `cp.c` | open/read/write |
| `clear` | `clear.c` | clear |
| `sleep` | `sleep.c` | sleep |
| `kill` | `kill.c` | kill |
| `ps` | `ps.c` | open/read `/proc/tasks` |
| `uname` | `uname.c` | open/read `/proc/version` |
| `uptime` | `uptime.c` | open/read `/proc/uptime` |

`/sbin/init` is `init.c`: it spawns `/bin/sh`, waits for it, and restarts it if
it dies. The boot chain is kernel → `kinit` (pid 1, kernel thread) → `/sbin/init`
(pid 2, Ring 3) → `/bin/sh` (pid 3).

## Shell builtins

Only operations that change the shell's own state stay in `sh.c`:

| Builtin | Why it cannot be a child |
|---|---|
| `cd` | changes *this* process's cwd (per-process, SYS_CHDIR) |
| `exit` | terminates *this* process |
| `exec` | replaces *this* process's image |
| `wait` | reaps *this* shell's background children |
| `help` | pure shell text |

`free`, `mem`, `cpuinfo`, `dmesg`, `mounts`, `net`, `devices`, `pci`, `date`
remain as one-word aliases for `cat /proc/<x>`; they are read-only views, not
kernel logic.

## Shell syntax

```
cmd  args...          run /bin/cmd (or a builtin)
cmd &                 background; `wait` reaps it later
a | b | c             pipeline: a real pipe and two more processes per stage
cmd > file            stdout to a new file
cmd >> file           stdout appended
cmd < file            stdin from a file
```

Redirection and pipelines both go through `SYS_SPAWNFDS(name, argv, in, out,
err)`: the shell opens the file or pipe, then hands the child its fds 0/1/2.
There is no `fork`/`dup2` — `SPAWNFDS` is the minimal stand-in (audit #13).

## User library (`ulib`)

`ulib.c` is the tiny libc: `strlen strcmp strncmp strcpy strncpy memcpy memmove
memset memcmp atoi uprintf uputs ugetline uerrstr ufail`, plus the inline
syscall wrappers in `ulib.h`. It contains no kernel code and links no kernel
symbols.

## Not done yet

- `ping` and DNS are still kernel-side; exposing them needs network syscalls
  (audit #31 / roadmap phase 9).
- No `malloc`/`free` in user space yet — programs use static buffers.
- No `dup`/`dup2`; `SPAWNFDS` covers the shell's needs.
- Programs are still embedded in the kernel as a boot fallback; a populated
  `disk.img` so `/bin` is the only source is planned (audit #3, #15).
