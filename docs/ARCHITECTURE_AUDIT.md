# VibeCagOS — Architecture Audit

> **Audit date:** 2026-10-07
> **Basis:** source read of every tracked file, not the README. Every claim below
> was checked against the code and, where marked *(verified)*, against a QEMU
> serial run (`make test-all`, 12 suites).
> **Scope:** describes the kernel as it exists now, including its known defects.

---

## 1. Boot flow

```
BIOS -> GRUB (Multiboot v1) -> loads kernel.elf at 0x100000
  |
boot.s:_start                      cli; esp = __stack_top; eflags = 0
  |
kernel_main(mb_magic, mb_info)     src/kernel/kernel.c
  +-- memset __bss
  +-- vga_init / serial_init / print_splash
  +-- klog_init                    ring buffer; readable via /proc/dmesg
  +-- pmm_init(__free_ram, __free_ram_end)
  +-- gdt_init                     7 descriptors + TSS  (kernel.c)
  +-- idt_init                     0x00-0x2F DPL0, 0x80 DPL3   (kernel.c)
  +-- pic_init                     remap to 0x20/0x28; IRQ0,1,2,12
  +-- pit_init(100)  +-- sti()
  +-- paging_init -> vmm_init      identity map, CR0.PG | CR0.WP
  +-- keyboard_init / rtc_init / kmalloc_init
  +-- ide_init / pci_init / nic_init / udp_init
  +-- vfs_init, vibefs mount at "/", procfs at /proc, install_user_programs()
  +-- devfs at /dev
  +-- process_init                 idle (pid 0)
  +-- process_create_kthread kinit (pid 1)
  +-- process_start()              never returns
          |
     switch_context -> idle -> kinit
          |
     kinit_main(): kernel_spawn_path("/sbin/init") [embedded-sh fallback],
                   process_wait(), power_off()
          |
     init (Ring 3, pid 2): spawn "/bin/sh", waitpid, propagate status
          |
     sh (Ring 3, pid 3): the interactive shell
```

`run_shell()` still exists but is only reachable through `make KSHELL=1`
(`#ifdef KSHELL_DEBUG`). It is not on the default boot path. *(verified)*

## 2. Memory map and ownership

| Region | Physical | Owner | Paging |
|---|---|---|---|
| 0x00000000–0x000FFFFF | BIOS / VGA 0xB8000 | firmware | kernel, supervisor |
| 0x00100000 | kernel image (`__kernel_base`) | kernel | kernel, supervisor |
| `.bss_end` + 128 KiB | boot stack (`__stack_top`) | kernel | kernel |
| `__free_ram`–`__free_ram_end` | 64 MiB PMM pool | kernel | kernel |
| 0x10000000 `USER_BASE` | user image | process | **user** |
| 0x1FFFC000–0x20000000 | user stack (4 pages + guard) | process | **user** |
| 0x20000000–0xC0000000 | reserved for user growth | — | unmapped |
| 0xC0000000+ | MMIO | kernel | supervisor |

`vmm_map_page()` silently strips `PAGE_USER` outside `[USER_BASE, USER_END)` and
logs an error, so a kernel address cannot become user-accessible by mistake.
`CR0.WP` is set, so a read-only PTE is enforced against ring 0 as well.
*(verified: `faults` suite, user read of 0x00100000 faults)*

**Ownership.** `pmm_alloc_frame()` hands out zeroed frames from a bitmap. Each
process owns the frames its user pages point at; `vmm_destroy_address_space()`
frees every user page, every user page table and the directory, and is the only
thing that may free an address space. Kernel page tables are *shared* (copied by
PDE reference), never freed.

## 3. Segments, TSS and the privilege boundary

| Selector | Descriptor | Used by |
|---|---|---|
| 0x08 / 0x10 | kernel code / data, DPL 0 | Ring 0 |
| 0x18 / 0x20 | user code / data, DPL 3 | Ring 3 (`0x1B`/`0x23` with RPL 3) |
| 0x28 | TSS, `iomap_base = sizeof(tss)` | `ltr` at boot |

The TSS has no I/O permission bitmap, so every bitmap bit beyond the segment
limit reads as 1: ring 3 is denied *all* port I/O. *(verified: `utest 9`)*

`TSS.esp0` is rewritten to the top of the current process's kernel stack on
every context switch, so any ring 3 → ring 0 transition lands on a private
stack and the full user context is saved there.

## 4. Interrupt path

`interrupts.s` normalises every vector into one frame:

```
low  gs fs es ds | edi esi ebp esp(dummy) ebx edx ecx eax | int_no err_code
high eip cs eflags | [user_esp user_ss]   <- last two words only if cs&3 == 3
```

`isr_common` builds it, reloads the kernel data selector, and calls
`handle_interrupt(f)`; `trapret` unwinds it and `iret`s. Vectors 0–47 use DPL 0
(`0x8E`), vector 128 uses DPL 3 (`0xEE`), so user code executing `int $0x20`
takes a #GP instead of reaching the timer ISR. *(verified: `utest 10`)*

`handle_interrupt` dispatch: 14 → `vmm_page_fault_handler`, <32 →
`handle_exception`, 32 → timer, 33 → keyboard, 44 → mouse, 128 →
`syscall_dispatch`; every other IRQ is acknowledged and dropped.

## 5. Timer and preemption

PIT channel 0 at 100 Hz → vector 32 → `ticks++` → EOI → poll serial for input →
`process_tick(f)`. `process_tick` wakes sleepers whose deadline passed and, when
the trap came from ring 3, decrements the quantum and calls `schedule_locked()`
once it hits zero.

**Preemption rule: kernel mode is not preemptible.** A ring 3 process can be
preempted at any instruction; a syscall or kernel thread gives up the CPU only
at an explicit `yield`/`block_on`/`sleep_ms`. That is a deliberate choice — it
means no kernel code has to be re-entrant against a timer.

## 6. Scheduler and context switch

`schedule_locked()` (IF=0): mark the current process RUNNABLE, walk `procs[]`
round robin from the next slot, RUNNABLE wins over `idle`, swap CR3, rewrite
`TSS.esp0`, then `switch_context(&prev->sp, next->sp)`.

`switch_context` is the xv6-style primitive in `interrupts.s`: it pushes
`ebp/ebx/esi/edi`, stores ESP, loads the other stack's ESP, pops and `ret`s. It
is only ever called from a place where the caller never returns (the scheduler
or a fresh-process bootstrap frame). The trick that makes this safe is that the
interrupt frame lives on the *per-process kernel stack*, so a preempted process
simply keeps its frame and resumes through it later. A new process gets a
hand-built iret frame plus a fake `switch_context` frame returning into
`trapret`.

Quantum: `TIME_SLICE_TICKS` = 3 ticks (30 ms). *(verified: `scheduler` suite —
two busy loops that never call yield interleave A/B/A)*

## 7. Process model

One thread per process. `struct process` (kernel.h) holds pid/ppid/state, the
saved kernel `sp`, the page directory, sleep deadline, wait channel, quantum,
exit code, entry/esp, name, the fd table (`OPEN_MAX` 16) and a per-process cwd,
plus a 32 KiB kernel stack with a canary at its bottom, checked on every switch.

States: `UNUSED CREATED RUNNABLE RUNNING SLEEPING BLOCKED ZOMBIE`.
`PROCS_MAX` is 16. Exit goes to `ZOMBIE` and is reaped by `waitpid()` or, for
orphans (`ppid == -1`), by the idle thread. *(verified: `proc` suite, including
that `FramesFree` is identical before and after a spawn/kill/orphan cycle)*

## 8. Syscalls

`int 0x80`, `EAX` = number, `EBX/ECX/EDX/ESI/EDI` = args, result in `EAX`.
Numbers and per-call contracts live in `src/abi/syscall.h`; the dispatcher is
`syscall_dispatch()` in `syscall.c`; file calls are in `sysfile.c`; program
loading is in `exec.c`. A negative return is `-errno`.

## 9. User memory safety

`usercopy.c` is the only way a user pointer is touched. `copy_from_user()`,
`copy_to_user()`, `strncpy_from_user()` and `user_range_valid()` check, for
**every** page of the range: inside `[USER_BASE, USER_END)`, no wrap-around,
PDE and PTE present, `PAGE_USER` set, and `PAGE_WRITE` set for destinations.
Nothing is copied unless the whole range validates. *(verified: `usercopy`
suite — NULL, kernel address, unmapped, wrap, cross-page, read-only)*

## 10. Programs and filesystems

User programs are VBIN executables (docs/VBIN.md): a 28-byte header, a
read-only text blob and a read-write data blob. Host ELF is a build
intermediate only — `tools/vbinpack` converts the linked output and the kernel
has no ELF loader. `src/kernel/vbin.c` validates the header against the real
image size and the user window, maps text RO and data RW, copies exactly the
file-backed bytes, and leaves BSS zero.

`spawn("name")` uses the built-in table (`progs.c`, images `.incbin`'d into the
kernel); `spawn("/bin/x")` and `exec("/bin/x")` read the file through the VFS.
Both end in the same `vbin_load()`. `exec` replaces the address space and CR3,
keeps fds 0/1/2, closes the rest, and enters the new image through a fresh iret
frame on the kernel stack. *(verified: `exec` suite, including 12 malformed
images that must all fail with `-ENOEXEC` without harming the caller)*

VFS layer over VibeFS (inode/directory tree on the IDE disk), procfs (`/proc`)
and devfs (`/dev`).

## 11. Critical CPU flows

```
A. user -> syscall
   Ring3 | int $0x80 -> CPU loads SS0:ESP0 from TSS, pushes ss/esp/eflags/cs/eip
         -> isr128 -> isr_common (frame + kernel DS)
         -> handle_interrupt -> syscall_dispatch -> kernel subsystem
         -> f->eax = result -> trapret -> iret -> Ring3

B. user -> timer -> another process
   Ring3 | IRQ0 -> TSS.esp0 -> isr32 -> isr_common (frame on THIS process's
         kernel stack) -> handle_interrupt: ticks++, EOI, process_tick
         -> schedule_locked -> switch_context (kernel stacks swapped, CR3 and
            TSS.esp0 updated) -> the next process resumes up ITS OWN call chain
         -> trapret -> iret -> Ring3 of the next process

C. user -> filesystem
   shell -> libc wrapper -> int $0x80 -> syscall_dispatch -> sys_open
         -> usercopy (path validated) -> path_resolve -> VFS -> VibeFS -> IDE

D. keyboard
   PS/2 -> IRQ1 -> isr33 -> keyboard_handle_irq -> kb ring buffer
         -> wakeup(console_chan) -> blocked SYS_READ wakes -> copy_to_user -> sh
```

## 12. Kernel / user dependency rule

`src/user/*` includes only `src/abi/syscall.h` and its own `ulib.*`. It cannot
reach `vfs_open`, `kmalloc`, `pmm`, `vmm`, `outb`, `cli` or VGA: none of those
symbols are linked into a user image, and ring 3 cannot call them anyway.

## 13. Known defects and limitations

| # | Issue | Severity |
|---|---|---|
| 1 | `exec` abandons the kernel C frames below ESP; the memory is not reclaimed until the process dies | low |
| 2 | `exec_image` is a 512 KiB static buffer, so a second exec cannot overlap the first | low, by design |
| 3 | User programs are still linked into the kernel image (`userblob.s`) as a boot fallback, but the disk is authoritative: bare names try `/bin` first, kinit starts `/sbin/init` from the VFS | medium (documented staging) |
| 4 | ~~`mkdir`, `rm`, `mv`, `cp`, `stat`, `ps`, `head`, `hexdump` are still shell builtins~~ resolved: every command except the five shell-control builtins is a `/bin` program | — |
| 5 | ~~a builtin cannot sit in a pipeline~~ resolved: `ls /bin \| head` and `echo x \| cat` are two-program pipelines | — |
| 6 | `PF_X` is parsed but not enforced: 32-bit paging without PAE has no NX bit | low, inherent |
| 7 | ~~`vibefs_alloc_block()` rebuilds a bitmap per block~~ resolved: the used-block bitmap is built once at mount and maintained on alloc/free | — |
| 8 | No permission enforcement in VibeFS: `mode` is stored but never checked | medium |
| 9 | Single-CPU only: no locking anywhere; `run-smp` exists but schedules one CPU | low |
| 10 | `struct process` embeds a 32 KiB stack, so 16 processes cost 512 KiB of BSS | low |
| 11 | ~~Maximum file size is 60 KiB~~ resolved: direct + single + double indirect, ~8 MiB max file on a 16 MiB disk | — |
| 12 | Kernel page faults are fatal even when a user process caused them indirectly | low |
| 13 | No `dup2` (fd inheritance is done at spawn time instead) and no signal handling; `kill` is immediate | low |
| 14 | The GUI (`src/drivers/gui.c`) still runs in ring 0 | see roadmap |
| 15 | `/bin` is written at boot from the embedded copies, so a program deleted from the disk comes back | low |

## 14. IPC

`pipe()` is the first IPC primitive: a kernel-owned ring buffer reference
counted by its open ends, blocking on the process wait channel with the pipe
address as the channel. `SYS_SPAWNFDS(name, argv, in, out, err)` lets the shell
hand a child its own pipe ends, which is how `cmd1 | cmd2` and `cmd > file`
work for programs. *(verified: `test-pipe` — bytes crossing a pipe between two
Ring-3 processes, EOF when the writer exits, `ls /bin | cat`)*

## 15. Migration status

Milestones 0–14 of the plan are done and covered by tests, plus the utility
migration: every non-shell-control command is now a `/bin` program (see
`docs/USERSPACE.md`). See `docs/ROADMAP.md` for what is left. The two
structural items that were still open when this audit was first written — the
executable loader and `exec` — landed as an ELF loader first ("exec: ELF32
loader...") and was then consolidated into the native VBIN format ("vbin:
..."), with fd inheritance and the first non-shell utilities in between.