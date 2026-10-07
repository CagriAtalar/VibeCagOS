# VibeCagOS — Feature Status

> **Updated:** 2026-10-07 · **Build:** clang/lld, 0 warnings · **Tests:**
> `make test-all`, 11 suites, 0 failures
>
> Every row below was read out of the source or asserted by a test. Where a
> feature exists but is incomplete, it says so.

## Boot

| Feature | State | Notes |
|---|---|---|
| GRUB Multiboot v1, ISO boot | done | `grub-mkrescue`, verified in QEMU |
| Boot splash + subsystem checklist | done | VGA text mode 80x25 |
| Multiboot memory info logged | done | `mem_lower`/`mem_upper` in klog |
| UEFI | not done | GRUB provides BIOS only |

## Kernel architecture

| Feature | State | Notes |
|---|---|---|
| 32-bit protected mode, flat segmentation | done | i386 |
| GDT: null / kcode / kdata / ucode / udata / TSS | done | selectors 0x08, 0x10, 0x18, 0x20, 0x28 |
| Ring-3 descriptors with DPL 3 | done | user CS `0x1B`, user DS `0x23` |
| TSS with per-process `esp0`, no I/O bitmap | done | ring 3 denied all port I/O (tested) |
| IDT: 0x00–0x1F exceptions, 0x20–0x2F IRQs, 0x80 syscall | done | DPL 0 except 0x80 (DPL 3) |
| Trap frame covering kernel- and user-originated traps | done | layout documented in `interrupts.s` |
| Kernel-mode faults panic, user-mode faults kill the process | done | `test-faults` |
| SMP | not done | `run-smp` boots a second CPU that nothing schedules |

## Memory

| Feature | State | Notes |
|---|---|---|
| Bitmap physical frame allocator | done | zero-filled frames, count in `/proc/meminfo` |
| 2-level paging, identity mapped kernel | done | `CR0.PG` + `CR0.WP` |
| Per-process page directories | done | kernel PDEs shared, user PDEs private |
| Kernel heap (kmalloc/kfree/coalesce/aligned) | done | `heap`, `/proc/meminfo` |
| User pages confined to `[USER_BASE, USER_END)` | done | enforced in `vmm_map_page` |
| ELF32 loader, R/W/X per `PT_LOAD` | done | `src/kernel/elf.c`, `test-exec` |
| Physical memory discovery | not done | 64 MiB pool is a linker constant |
| Copy-on-write, demand paging, PAE/NX | not done | out of scope for this milestone |

## Processes and scheduling

| Feature | State | Notes |
|---|---|---|
| Preemptive round-robin at 100 Hz | done | quantum 3 ticks, verified with two yield-free busy loops |
| Kernel mode non-preemptible | done | by design; kernel gives up CPU at yield/block/sleep |
| Explicit states incl. SLEEPING / BLOCKED / ZOMBIE | done | see `kernel.h` |
| sleep / wakeup / yield | done | tick-based deadlines |
| Per-process kernel stacks with canaries | done | 32 KiB each |
| spawn / waitpid / kill / orphan reaping | done | `test-proc`, no frame leak |
| Per-process cwd | done | paths normalised in `sysfile.c` |
| Per-process fd tables (0/1/2 = console) | done | `test-stdio`, `test-fs` |
| exec (address-space replacement) | done | `test-exec` |
| Threads, priorities, SMP scheduling | not done | one thread per process by design |

## Syscalls and user memory

| Feature | State | Notes |
|---|---|---|
| `int 0x80` dispatcher with a central ABI header | done | 27 syscalls, `src/abi/syscall.h` |
| Validated `copy_from_user` / `copy_to_user` / `strncpy_from_user` | done | per-page checks, no partial copies |
| Unknown syscall → `-ENOSYS`, bad fd → `-EBADF` | done | |
| Errors as `-errno` | done | VFS errors translated in `sysfile.c` |

## Filesystems and I/O

| Feature | State | Notes |
|---|---|---|
| VFS with mount table and vnodes | done | |
| VibeFS: inodes, directories, `.`/`..`, 60 KiB max file | done | on the IDE disk (PIO) |
| procfs (`/proc/*`) | done | tasks, meminfo, uptime, dmesg, pci, devices, date |
| devfs (`/dev/*`) | done | null, zero, console, random, tty |
| PS/2 keyboard IRQ1 → ring buffer → blocking `read` | done | |
| PIT 100 Hz, PIC remap | done | |
| Permission enforcement in VibeFS | not done | `mode` stored, not checked |
| Indirect blocks / large files | not done | 60 KiB ceiling |

## Programs

| Feature | State | Notes |
|---|---|---|
| User-space shell (`sh`) as an ordinary Ring-3 process | done | `kernel_main` no longer calls `run_shell` |
| Shell uses only syscalls, includes no kernel header | done | |
| ELF user programs in `/bin` | done | installed at boot from embedded images |
| Pipes as the first IPC primitive | done | `test-pipe` |
| fd inheritance / `dup2` / shell pipelines `\|` | not done | blocks milestone 14 |
| Programs still embedded in the kernel image | staging | see roadmap |

## Devices, networking, GUI

| Feature | State | Notes |
|---|---|---|
| IDE/ATA PIO, PCI scan, RTL8139 | done | |
| Ethernet / ARP / IPv4 / ICMP ping / UDP / DNS | done | polling NIC, commands behind procfs |
| Keyboard history, arrows, backspace | done | raw TTY, edited in user space |
| VGA text output to serial | done | |
| Framebuffer, compositor, window manager | not done | GUI is milestones 15–16 |
| `src/drivers/gui.c` (a ring-0 demo desktop) | legacy | not part of the userspace design |

## Test coverage

11 suites, run headless against the serial console: `ring3`, `syscall`,
`scheduler`, `usercopy`, `faults`, `stdio`, `fs`, `shell`, `pipe`, `proc`,
`exec`. They assert privilege level, register state, preemption without yield,
pointer validation, page-fault containment, fd semantics, filesystem round
trips, zombie/orphan lifetime and ELF/exec behaviour — not just that the OS
boots.