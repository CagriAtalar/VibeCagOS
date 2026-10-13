# VibeCagOS — Architecture

> How the system fits together. For the memory picture see
> [`MEMORY_MAP.md`](MEMORY_MAP.md); for the syscall ABI see
> [`SYSCALLS.md`](SYSCALLS.md); for decisions and their rationale see
> [`DECISIONS.md`](DECISIONS.md).

VibeCagOS is a 32-bit x86 (i386) protected-mode kernel with a strict Ring-3
userland. It boots via GRUB (Multiboot v1) and runs on QEMU.

## Layers

```
 Ring 3   init (pid 1) ──▶ sh ──▶ coreutils (ls cat echo cp mv rm mkdir …)
             ▲                    utest (self-test battery)
             │  libc-lite: src/user/ulib.{c,h} (syscall stubs, str*, atoi)
 ────────────┼───────────────────────────────────────────────────────────────
 int 0x80   │  EAX=nr, EBX/ECX/EDX/ESI/EDI=args, return EAX (-errno on error)
 ────────────┼───────────────────────────────────────────────────────────────
 Ring 0   syscall dispatcher ── usercopy ── fd table ── VFS ── VibeFS ── ATA PIO
          process/scheduler  ── paging/PMM/heap ── interrupts/timer ── drivers
 Hardware  PIC/PIT, PS/2 keyboard, serial, VGA, PCI, RTL8139, IDE, RTC
```

## Boot flow

```
GRUB (Multiboot v1)
  │  loads kernel.elf at 0x100000, passes multiboot_info, enters _start
  ▼
_start (src/kernel/boot.s)
  │  temporary stack, save mb_info, enable paging, call kernel_main
  ▼
kernel_main (src/kernel/kernel.c)
  │  serial + VGA console, klog, GDT + TSS, IDT, PIC remap, PIT 100 Hz,
  │  keyboard, PCI scan, RTC
  │  kmalloc_init, vmm_init (paging), pmm_init(64 MiB frame window)
  │  VibeFS format/mount at /, procfs at /proc, devfs at /dev
  │  install embedded VBIN programs into /bin
  ▼
process_init()            idle thread = pid 0, slot 0
process_create_kthread("kinit", kinit_main)   = pid 1 (see ADR-008)
process_start()           never returns: switches to idle + kinit
  ▼
kinit_main → kernel_spawn_path("/sbin/init")
  ▼
init (pid 2 today, pid 1 after ADR-008) → spawns /bin/sh
  ▼
sh: prompt, read commands, spawn /bin programs, wait
```

`process_start()` abandons the boot stack and never returns; the idle thread
runs `hlt` whenever no process is runnable.

## Privilege model

- GDT has kernel code/data (DPL 0) and user code/data (DPL 3) plus a TSS whose
  `esp0` is updated to the current process's kernel-stack top on every switch.
- IDT DPL: only vector `0x80` is reachable from Ring 3; every hardware vector
  and CPU exception is DPL 0, so Ring 3 executing them gets `#GP`.
- Entering the kernel (syscall/IRQ/exception) loads SS0:ESP0 from the TSS and
  lands on the process's private kernel stack.
- `copy_from_user` / `copy_to_user` / `strncpy_from_user` validate that a range
  lies in `[USER_BASE, USER_END)` and is fully mapped before touching it; the
  kernel never dereferences a raw user pointer. This is tested by `test-usercopy`
  and `test-faults`.

## Processes and scheduling

- `struct process` (`src/kernel/kernel.h`): pid, ppid, state
  (`UNUSED/RUNNABLE/RUNNING/SLEEPING/ZOMBIE/BLOCKED/CREATED`), saved `sp`,
  `page_table`, `sleep_until`, `wait_chan`, `time_slice`, `exit_code`, entry
  EIP/ESP, per-process kernel stack, fd table, cwd.
- `PROCS_MAX = 16`, fixed pool `procs[]`. Context switch is
  `switch_context()` in `src/kernel/interrupts.s`: it saves/restores
  callee-saved registers and ESP, switches CR3, and repoints `TSS.esp0`.
- The PIT (100 Hz) drives preemption: on tick, `PROC_SLEEPING` deadlines are
  checked and `PROC_BLOCKED` wait channels are woken; the running process's
  `time_slice` (`TIME_SLICE_TICKS = 3` ticks ≈ 30 ms) is decremented and the
  scheduler round-robins to the next runnable process.
- Blocking: `block_on(channel)` / `wakeup(channel)` — the wait channel is just
  the address of a variable. Used by blocking console reads and pipes.
- The kernel is **non-preemptible**: no timer-driven switch happens inside
  kernel code; preemption only takes effect on return to Ring 3 or from the idle
  thread. Critical sections use `irq_save()` / `irq_restore()`.

## Syscall path

```
Ring 3: int 0x80
  → isr128 (interrupts.s) → isr_common (saves trap_frame) → handle_interrupt()
  → syscall_dispatch(frame) (src/kernel/syscall.c): switch on EAX, args a1..a5
  → e.g. sys_write → fd table → VFS → VibeFS → ATA PIO
  → result in frame->eax → trapret → iret → Ring 3
```

A syscall may block (read/sleep/yield): the trap frame lives on the process's
kernel stack, so it survives a context switch and is still valid on resume.
Unknown numbers return `-ENOSYS`; errors are returned as `-errno`. The ABI is
defined once in `src/abi/syscall.h`, shared verbatim by kernel and user.

## Program loading

`exec`/`spawn` read a **VBIN** image (embedded blob or a file), validate the
header, map RO text at `USER_BASE` and RW data after it, zero BSS, and build the
initial user stack with `argc`/`argv`. `exec` is transactional: it validates the
new image before tearing down the old address space. Fds 0/1/2 survive `exec`;
the rest are closed. See [`VBIN.md`](VBIN.md) and ADR-002.

## Filesystem

```
VFS (src/fs/vfs.c): superblock, mount table, vnode, struct file{offset,ops}
  ├─ VibeFS  /      disk: inodes, dirs, direct+indirect blocks, persistent
  ├─ procfs  /proc  meminfo, uptime, tasks, version, cpuinfo, …
  └─ devfs   /dev   null, zero, console, random, tty
```

Each process has its own fd table (`OPEN_MAX = 16`); fds 0/1/2 are the console.
Pipes are refcounted objects shared by their two ends. `SYS_SPAWNFDS` lets the
shell hand fds to a child (pipelines/redirection) without `fork`+`dup2`
(ADR-003). See [`FILESYSTEM.md`](FILESYSTEM.md).

## Drivers

- **Serial (COM1)** and **VGA text** console, keyboard (PS/2, set 1, shift/ctrl/
  caps, IRQ-driven ring buffer).
- **PIC/PIT** (remapped, 100 Hz), **RTC**.
- **ATA PIO** IDE driver for `disk.img` (16 MiB).
- **PCI** config scan; **RTL8139** NIC with an Ethernet/ARP/IPv4/ICMP/UDP/DNS
  stack (polling; no socket syscalls yet).
- **GUI** (`src/drivers/gui.c`) is a Ring-0 demo, not on the default boot path.

## Further reading

- [`RING3.md`](RING3.md) — privilege transitions and the syscall boundary.
- [`VBIN.md`](VBIN.md) — executable format and the packer.
- [`FILESYSTEM.md`](FILESYSTEM.md) — VibeFS on-disk format.
- [`SYSCALLS.md`](SYSCALLS.md) — syscall table.
- [`TESTING.md`](TESTING.md) — the test suites.
- [`AUDIT.md`](AUDIT.md) / [`ARCHITECTURE_AUDIT.md`](ARCHITECTURE_AUDIT.md) —
  state and known defects.
