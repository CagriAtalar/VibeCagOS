# VibeCagOS — Build, Run and Test Reference

> **Updated:** 2026-10-07, against the tree with the VBIN loader and `exec()`.
> Verified with clang/lld/GRUB/QEMU in WSL2 Ubuntu.

## Requirements

```
clang lld llvm-objcopy qemu-system-i386 grub-pc-bin grub-common xorriso
```

WSL2 Ubuntu 22.04+ works as-is; `clang` targets the host by default, so the
Makefile passes `-m32` explicitly.

## Build

```bash
make            # -> kernel.elf, sh.elf, utest.elf, os.iso
make clean
```

The build produces three kinds of artifact:

| Artifact | Built from | Loaded by |
|---|---|---|
| `kernel.elf` | `src/kernel`, `src/drivers`, `src/fs`, `src/net` | GRUB |
| `*.elf` (intermediate) | `src/user` via `src/user/user.ld` | `tools/vbinpack` (host) |
| `*.vbin` (runtime) | `*.elf` via `tools/vbinpack` | the kernel's VBIN loader |
| `os.iso` | `kernel.elf` + a GRUB menu | `grub-mkrescue` |

The user VBINs are additionally `.incbin`'d into the kernel image
(`src/kernel/userblob.s`) so a build boots without a pre-populated disk; the
kernel copies them into `/bin` (`/sbin` for init) at boot, after which programs
are also reachable as ordinary files. Explicit stages: `make kernel`,
`make user`, `make tools`, `make image`.

## Run

```bash
dd if=/dev/zero of=disk.img bs=1M count=2   # or just `make run`
make run          # serial console only
make run-window   # VGA window as well
make run-smp      # 2 CPUs (the second is not scheduled)
make debug        # QEMU halted with a GDB stub on :1234
```

## Tests

```bash
make test-all           # every suite below, ~4 minutes
make test-ring3         # Ring-3 entry, CS/SS RPL, exit status
make test-syscall       # dispatcher, ENOSYS, EBADF, sleep
make test-scheduler     # two busy loops interleaved by the timer, no yield
make test-usercopy      # copy_from/to_user against NULL, kernel, unmapped,
                        #   wrapping, cross-page and read-only pointers
make test-faults        # user page fault, #GP from cli/outb/int, RO text page
make test-stdio         # fd 0/1/2, blocking read, EMFILE, fd reuse
make test-fs            # open/read/write/lseek/stat/mkdir/unlink/readdir,
                        #   plus a 1.5 MiB file across the indirect blocks
make test-shell         # the user shell's filesystem commands
make test-pipe          # pipes: round trip, EOF, full buffer
make test-proc          # spawn/wait/kill/zombies/orphans, frame-leak check
make test-exec          # VBIN loading, malformed images, exec(), argv and fd rules
make test-persist       # write a file, reboot on the same image, read it back
```

`tests/run.sh <suite>` boots QEMU headless, types commands into the **user**
shell over the serial port, waits for each prompt, and greps the log. There are
no sleeps in the assertions: each command is sent only after the previous prompt
appeared, so the suites are not timing sensitive. `utest <n>` in the shell runs
the Ring-3 self tests in `src/user/utest.c`.

A failing suite keeps its log at `/tmp/vibe-last.log`.

## Memory assumptions

* Kernel image is linked at 0x100000 (`src/kernel/kernel.ld`).
* 128 KiB boot stack after `.bss`, then a 64 MiB PMM pool. `-m 128M` in QEMU is
  ample; the pool is a fixed linker-script constant, not detected from the
  multiboot memory map.
* VGA text buffer at 0xB8000, serial at 0x3F8 — both hard-coded.
* `disk.img` is 2 MiB (4096 sectors). VibeFS derives its data area from that
  number (`VIBEFS_TOTAL_SECTORS`), so changing the image size means changing
  one constant.

## GDB

```bash
make debug                       # terminal 1: QEMU, halted, stub on :1234
gdb kernel.elf                   # terminal 2
(gdb) target remote :1234
(gdb) set pagination off
(gdb) break trapret              # every ring-3 return
(gdb) continue
(gdb) info registers              # cs/ss & 3 == 3, eip in USER_BASE, esp on the stack
(gdb) x/12wx $esp                 # the iret frame
(gdb) p/x *(uint32_t*)($esp+8)    # eip / cs / eflags / user_esp / user_ss
```

Useful breakpoints: `isr_common`, `handle_interrupt`, `trapret`,
`schedule_locked`, `switch_context`, `syscall_dispatch`, `elf_load`,
`vmm_page_fault_handler`, `kernel_panic`.

Symbols are `-g3`. Kernel log lines carry a subsystem tag (`[PROC]`, `[SCHED]`,
`[VMM]`, `[IRQ]`, `[SYSCALL]`, `[EXEC]`) and are readable at runtime with
`dmesg` / `/proc/dmesg`.