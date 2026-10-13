# VibeCagOS — Feature Status

> **Updated:** 2026-10-13 · **Build:** clang/lld `-Werror`, 0 warnings · **Tests:**
> 12 suites via `tests/all.sh` / `make test-all`: 12 PASS (run in WSL2 Ubuntu,
> QEMU 8-era, disposable disk images; `disk.img` untouched)
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
| Kernel heap (kmalloc/kfree/coalesce) | done | merges only memory-contiguous blocks; overflow-safe sizes; OOM returns NULL (no panic) after init. `kmalloc_aligned` removed (unfreeable). No host/QEMU unit test of the heap itself yet (IMPLEMENTED_BUT_UNVERIFIED beyond the suites exercising it) |
| User pages confined to `[USER_BASE, USER_END)` | done | enforced in `vmm_map_page` |
| VBIN loader, text RO / data RW / BSS zero | done | `src/kernel/vbin.c`, `tools/vbinpack`, `test-exec`. Executable read into a per-load heap buffer (limit 16 MiB, no static buffer); span cap 16 MiB. Multi-MB images are NOT yet exercised by a test |
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
| `int 0x80` dispatcher with a central ABI header | done | 29 syscalls (`SYS_MAX`), `src/abi/syscall.h` |
| Validated `copy_from_user` / `copy_to_user` / `strncpy_from_user` | done | per-page checks, no partial copies |
| Unknown syscall → `-ENOSYS`, bad fd → `-EBADF` | done | |
| Errors as `-errno` | done | VFS errors translated in `sysfile.c` |

## Filesystems and I/O

| Feature | State | Notes |
|---|---|---|
| VFS with mount table and vnodes | done | |
| VibeFS: inodes, directories, `.`/`..`, ~8 MiB max file | done | on the IDE disk (PIO); direct + single + double indirect |
| procfs (`/proc/*`) | done | tasks, meminfo, uptime, dmesg, pci, devices, date |
| devfs (`/dev/*`) | done | null, zero, console, random, tty |
| PS/2 keyboard IRQ1 → ring buffer → blocking `read` | done | |
| PIT 100 Hz, PIC remap | done | |
| Permission enforcement in VibeFS | not done | `mode` stored, not checked |
| Indirect blocks / large files | done | single + double indirect; a 1.5 MiB file is written/read/truncated in `test-fs` (`T28`) |

## Programs

| Feature | State | Notes |
|---|---|---|
| User-space shell (`sh`) as an ordinary Ring-3 process | done | `kernel_main` no longer calls `run_shell` |
| Shell uses only syscalls, includes no kernel header | done | |
| VBIN user programs in `/bin` + `/sbin/init` | done | packed by `vbinpack`, installed at boot; disk is authoritative, embedded is fallback |
| Ring-3 init (`/sbin/init` spawns `/bin/sh`, propagates status) | done | boot is kinit → init → sh |
| Pipes as the first IPC primitive | done | `test-pipe` |
| fd inheritance at spawn (`SYS_SPAWNFDS`), shell pipelines `\|` | done | `ls /bin \| cat` |
| Utilities as separate VBIN programs | done | `ls cat echo pwd head hexdump stat touch write mkdir rmdir rm mv cp clear sleep kill ps uname uptime` are all in `/bin` |
| Shell-control builtins (`cd`, `exit`, `exec`, `wait`, `help`) | done | they must run in the shell's own process |
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

12 suites, run headless against the serial console: `ring3`, `syscall`,
`scheduler`, `usercopy`, `faults`, `stdio`, `fs`, `shell`, `pipe`, `proc`,
`exec`, `persist` (two boots on one image). They assert privilege level,
register state, preemption without yield, pointer validation, page-fault
containment, fd semantics, filesystem round trips, reboot persistence,
pipelines, zombie/orphan lifetime and VBIN/exec behaviour — not just that the
OS boots.
## Master-plan items NOT started (as of 2026-10-13)

x86_64 target / Limine-or-Multiboot2 boot / UEFI, real memory-map-driven PMM, 4-level paging + NX, threads, SMP, VBIN v2 (segment table), libc/malloc/mmap, dup/dup2, permission enforcement, virtio-blk, DHCP/TCP/sockets, framebuffer + compositor, Doom. State for all of these: NOT_IMPLEMENTED.

## Build hygiene

- `make clean` no longer deletes `disk.img`; `make distclean` does (destructive).
- `make disk.img` refuses to overwrite an existing image of the wrong size.
- Test targets no longer depend on `disk.img`; every suite uses a `mktemp` image.
- `tests/all.sh` runs all 12 suites. Version string lives only in `src/kernel/version.h`.

