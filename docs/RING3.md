# Ring 3 execution model (VibeCagOS)

## Address space (32-bit, 2-level paging)

| Range | Use | Access |
|---|---|---|
| `0 .. __free_ram_end` (~65 MB) | kernel, identity mapped, page tables **shared** by all processes | supervisor only |
| `0x10000000` `USER_BASE` | user image: one `PT_LOAD` per ELF permission class | user |
| `0x1FFFC000 .. 0x20000000` | user stack, 4 pages, page below unmapped (guard) | user RW |
| `.. 0xC0000000` `USER_END` | reserved for user heap | user |
| `0xC0000000 ..` | kernel-only (MMIO) | supervisor |

`vmm_map_page()` strips `PAGE_USER` outside `[USER_BASE, USER_END)` (and logs an
error), so a kernel address can never become user-accessible. User page tables
live in PDE slots 64..767, which are private per process; all other PDEs are
copied by reference from the master directory. `CR0.WP` is set, so ring 0 also
honours read-only PTEs.

## Trap frame (`struct trap_frame`, built by `isr_common`)

low -> high: `gs fs es ds | edi esi ebp esp(dummy) ebx edx ecx eax | int_no
err_code | eip cs eflags | [user_esp user_ss]`.

The last two words exist only when `cs & 3 == 3` (privilege change). Use
`TF_FROM_USER(f)`.

## Flows

**syscall**: `int 0x80` -> CPU loads `SS0:ESP0` from the TSS -> `isr128` ->
`isr_common` (saves frame, loads kernel DS) -> `handle_interrupt` ->
`syscall_dispatch(f)` -> `f->eax = ret` -> `trapret` -> `iret` -> Ring 3.

**timer preemption**: IRQ0 -> same entry path -> `handle_interrupt`: `ticks++`,
EOI, `process_tick(f)` (wakes sleepers; if `TF_FROM_USER(f)` and the quantum is
used up -> `schedule_locked()`) -> `switch_context()` swaps **kernel stacks** ->
the new process returns up *its own* call chain -> `trapret` -> `iret` to the
exact instruction where it was interrupted.

**why this is safe**: the interrupt frame is stored on the *per-process* kernel
stack (`TSS.esp0` is updated on every switch). `switch_context` only swaps
callee-saved registers and ESP; nothing of the old process is left half-finished
because its frame simply stays on its own stack until it is scheduled again. A
new process gets a hand-built frame + a fake `switch_context` frame returning
into `trapret`.

**preemption rule**: kernel mode is not preemptible. The timer only switches when
it interrupted Ring 3 (or the idle thread). Kernel threads/syscalls give up the
CPU at `yield`/`block_on`/`sleep_ms`.

## User pointers

Never dereferenced by the kernel. `copy_from_user / copy_to_user /
strncpy_from_user / user_range_valid` (`usercopy.c`) validate range,
wrap-around, PDE+PTE present, `USER` bit and `WRITABLE` (for writes) for
**every** page, before copying anything (no partial copies).

## Tests (`make test-all`)

`tests/run.sh <ring3|syscall|scheduler|usercopy|faults|stdio|fs|shell|proc|exec>`
boots QEMU headless, drives the user shell over serial and greps the log. Test
programs: `src/user/utest.c`.

## File descriptors (M8/M9)

`user fd -> current_proc->fds[fd] -> { console | struct file } -> VFS -> VibeFS -> IDE`

* fd 0 = stdin (console, read-only, blocking), fd 1/2 = stdout/stderr (console,
  write-only), bound at process creation. The console is the kernel TTY: keyboard
  IRQ1 + serial poll in, VGA + serial out. Ring 3 never touches VGA or port 0x60.
* New fds are the lowest free slot >= 3, `OPEN_MAX` = 16 (`-EMFILE` when full).
  All fds are closed at process exit/kill.
* Syscalls: OPEN CLOSE READ WRITE LSEEK STAT FSTAT MKDIR UNLINK RMDIR READDIR
  (contracts in `src/abi/syscall.h`). Errors are `-errno` (VFS errors are
  translated in `sysfile.c`). Paths are copied with `strncpy_from_user`.
* Known limits: no `dup2` — inheritance is done by `SYS_SPAWNFDS` at spawn time
  instead; `O_RDONLY` write -> `-EBADF` (POSIX).

## Two bugs found while stress-testing (both pre-existing, now fixed)

* `klog()` ended with an unconditional `sti()`: it enabled interrupts during
  early boot (before the PIC was remapped -> IRQ0 hit vector 8 = double fault)
  and inside ISRs. It now uses `irq_save`/`irq_restore`.
* The Ring-0 shell moved from the large boot stack to a process kernel stack;
  8 KiB overflowed into the neighbouring stack. `KERNEL_STACK` is 32 KiB and a
  canary at the bottom of every stack is checked on each context switch
  (`kernel stack overflow` panic instead of silent corruption).

## User-space shell, spawn/wait, cwd, argv (M10/M11)

**Boot flow now:** `kernel_main -> process_init -> kinit (kernel thread, pid 1)
-> spawn "sh" (Ring 3) -> process_wait`. `run_shell()` is no longer on the boot
path (it only runs in the `make KSHELL=1` debug build). When `sh` exits, kinit
powers the machine off.

**Process creation:** `process_create_user(name, image, size, argc, argv)` builds
the initial user stack (argc / argv[] / strings, System-V style); `crt0.s`
passes `(argc, argv)` to `user_main()`.

**Syscalls added:** SPAWN, WAITPID, CHDIR, GETCWD, RENAME, KILL, CLEAR, PIPE,
EXEC (contracts in `src/abi/syscall.h`).

* `spawn(name, argv)` returns the child's pid; the child inherits the parent's
  cwd; fds 0/1/2 = console.
* `waitpid(pid|-1, &status)` blocks on the *parent* (`block_on(parent)`, woken by
  `make_zombie(child)`), reaps the zombie and frees its address space;
  `-ECHILD` if there are no children.
* If a parent exits first its children are orphaned (`ppid = -1`); the idle
  thread reaps them when they die.
* cwd is per process; `sysfile.c` makes every path absolute and normalises
  `.`/`..` before it reaches the VFS, so the VFS's own global cwd is not used.

**Shell (`src/user/sh.c`)** includes no kernel header. Builtins use syscalls only;
system information comes from procfs. Non-builtins are spawned and waited for;
`&` runs them in the background and `wait` reaps them. `>` / `>>` and `|` work
for programs as well as builtins.

## fd inheritance and pipelines (M13)

`SYS_SPAWN` always gave the child a fresh console, so nothing could be
redirected. `SYS_SPAWNFDS(name, argv, in, out, err)` takes the child's fds 0/1/2
from the caller's descriptors (`-1` closes that fd) and closes everything else
in the child's table — the toy-OS stand-in for `fork()` + `dup2()`. Pipe ends
are reference counted per end, so a child's close cannot free a pipe the parent
is still holding.

The shell uses it for `cmd > file` and for `cmd1 | cmd2 | cmd3`: one pipe per
stage, the shell dropping its own copies right after each spawn, otherwise the
reader would never see EOF.

## Programs as separate files (M14)

`ls`, `cat` and `echo` are no longer shell builtins: they are ELF programs in
`/bin` (`src/user/{ls,cat,echo}.c`), loaded by the same ELF loader and spawned
like anything else. `cat` with no arguments reads fd 0, so `utest 25 | cat` is a
genuine two-process pipeline.

## ELF32 programs and exec (M12)

User programs are ELF32 executables, not flat blobs. `src/user/user.ld` gives one
`PT_LOAD` per permission class (`.text` R+X, `.rodata` R, `.data`/`.bss` RW), so
the loader needs no special cases and no dynamic linker (`ET_DYN` is refused).

`src/kernel/elf.c` validates the header and every program header against the real
image size and the user window, maps the segments, copies `p_filesz` bytes, and
demotes pages that no `PF_W` segment covers back to read-only (`.bss` is left
zero because `pmm` hands out zeroed frames). `PF_X` is parsed but **not**
enforced: 32-bit paging without PAE has no NX bit.

Two entry points, one loader:

* `spawn("utest")` -> built-in image in `progs.c` (`.incbin`'d by `userblob.s`)
* `spawn("/bin/utest")` / `exec("/bin/utest")` -> read through the VFS

At boot `install_user_programs()` copies the built-in images into `/bin`, so
programs exist as ordinary files on a plain `disk.img`.

`exec()` (in `exec.c`) replaces the calling process's address space:

1. read + validate the ELF into a scratch buffer — nothing is mapped yet, so a
   bad path or a bad ELF returns `-ENOENT`/`-EACCES`/`-ENOEXEC` and the caller
   keeps running
2. build a new address space, load the ELF, map a fresh user stack
3. keep fds 0/1/2, close everything else (POSIX)
4. swap `current_proc->page_table` and `CR3`, then free the old directory
5. `exec_return_to_user()` builds the same "return into `trapret`" frame a new
   process gets and switches onto it; the C frames below ESP are abandoned and
   never read again

## Known limits

* VibeFS caps files at 60 KiB (120 direct blocks); larger executables need
  indirect blocks.
* `exec` reuses a single 512 KiB static image buffer, so two execs cannot overlap.
* `PF_X` is not enforced (no NX bit in 32-bit paging).
* Programs are still embedded in the kernel image and copied to `/bin` at boot;
  they are not built into the disk image, so deleting one from `/bin` brings it
  back on the next boot.