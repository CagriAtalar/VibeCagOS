# VibeCagOS — Roadmap

> **Current Version:** 0.2.0  
> **Status:** Phase 1 complete (Foundation), Phase 2 in progress

---

## Completed Work (v0.2.0)

### Phase 1 — Foundation ✅

- [x] Architecture audit and documentation
- [x] **GDT** — proper 6-entry: null, kernel code/data, user code/data, TSS
- [x] **TSS** — Task State Segment for ring3→ring0 kernel stack
- [x] **IDT** — all CPU exceptions (0-14), PIC IRQs (0x20-0x2F), syscall (0x80)
- [x] **PIC** — 8259 remapped to 0x20/0x28, IRQ0 + IRQ1 unmasked
- [x] **PIT Timer** — 100 Hz preemptive tick
- [x] **Paging** — identity map enabled, CR0.PG set
- [x] **PS/2 Keyboard** — scancode decoder with ring buffer, shift support, arrow keys
- [x] **Panic screen** — register dump, CPU state, halt
- [x] **Exception handlers** — divide-by-zero, page fault (with CR2), GPF
- [x] **Preemptive scheduler** — round-robin, SLEEPING/ZOMBIE states
- [x] **sleep_ms()** — tick-based sleep primitive
- [x] **VGA colors** — full 4-bit color attribute support
- [x] **Colored shell** — green prompt, color-coded commands
- [x] **Command history** — up/down arrow navigation
- [x] **Boot splash** — ASCII art banner with version
- [x] **SimpleFS bug fix** — first-file write bug corrected
- [x] **Improved block allocator** — O(n) bitmap scan vs O(n^2)
- [x] **printf improvements** — left-justify, zero-pad, field width, %p
- [x] **Shell commands** — help, hello, uname, uptime, mem, ps, ls, cat, create, write, rm, format, ping, clear, exit
- [x] **Build system** — proper separate compilation, debug/disassemble targets
- [x] **Documentation** — ARCHITECTURE_AUDIT.md, BASELINE.md

---

## In Progress

### Phase 2 — Memory Management

- [ ] Proper physical frame allocator (bitmap, not bump)
- [ ] `kfree()` / kernel heap
- [ ] Physical memory statistics (total, used, free by region type)

### Phase 3 — Scheduler Improvements

- [ ] Scheduler statistics (context switches, tick count per process)
- [ ] Per-process runtime accounting
- [ ] `kill` command to terminate processes

---

## Planned

### Phase 4 — Ring 3 Userspace

- [ ] Launch user processes in ring 3 from shell
- [ ] ELF loader for simple executables
- [ ] User stack setup
- [ ] Complete syscall validation (user pointer bounds checking)
- [ ] `copy_from_user` / `copy_to_user`
- [ ] `/sbin/init` as first userspace process

### Phase 5 — Filesystem Improvements

- [ ] Subdirectory support
- [ ] Larger files (indirect block pointers)
- [ ] `append` mode
- [ ] `rename` command
- [ ] File permissions
- [ ] Current working directory (`cd`, `pwd`)

### Phase 6 — TTY / Terminal Improvements

- [ ] ANSI escape code support (colors from userspace)
- [ ] Terminal resize
- [ ] Multiple virtual terminals
- [ ] Pipe between shell commands

### Phase 7 — IPC

- [ ] Anonymous pipes
- [ ] Named pipes (FIFOs)
- [ ] Signals (SIGKILL, SIGTERM)

### Phase 8 — GUI (Graphical Mode)

- [ ] VESA/BIOS framebuffer (800x600 or 1024x768)
- [ ] 2D drawing primitives (pixel, rect, line, blit)
- [ ] Double buffering
- [ ] Font rendering (bitmap font)
- [ ] Simple window manager
- [ ] Desktop with taskbar and clock
- [ ] Terminal window application
- [ ] System monitor application
- [ ] File browser application

### Phase 9 — SMP (Optional)

- [ ] ACPI table parsing (MADT for CPU count)
- [ ] APIC initialization
- [ ] Application Processor startup
- [ ] Per-CPU scheduler queues
- [ ] IPI (inter-processor interrupts) for scheduling

### Phase 10 — Networking

- [x] RTL8139 driver (done)
- [x] Ethernet frame handling (done)
- [x] ARP (done)
- [x] IPv4 (done)
- [x] ICMP ping (done)
- [ ] UDP socket abstraction
- [ ] DNS resolver
- [ ] TCP (basic)

### Phase 11 — Security Hardening

- [ ] SMEP/SMAP enable on supported CPUs
- [ ] NX (no-execute) for data regions
- [ ] ASLR (address space layout randomization)
- [ ] Stack canaries
- [ ] User pointer validation in all syscalls
- [ ] Syscall argument type checking

### Phase 12 — Advanced Memory

- [ ] Copy-on-write fork()
- [ ] Demand paging (lazy allocation)
- [ ] mmap() system call
- [ ] Shared memory
- [ ] Page cache for filesystem

### Phase 13 — CI/CD

- [ ] GitHub Actions workflow
- [ ] Build matrix (different toolchain versions)
- [ ] QEMU boot test in CI
- [ ] Serial output verification
- [ ] Kernel panic detection in CI
- [ ] Host-side unit tests (string, path, allocator)

### Phase 14 — Applications

- [ ] `ed` or simple text editor
- [ ] `hexdump` utility
- [ ] Calculator
- [ ] Network diagnostic tools (ifconfig, route)

---

## Milestones

| Milestone | Description | Target |
|---|---|---|
| v0.1.0 | Original — GRUB + shell + SimpleFS | Done |
| v0.2.0 | GDT/IDT/PIC/PIT/KB/paging/colors/history | Done |
| v0.3.0 | Ring 3 + ELF loader + full syscall table | Planned |
| v0.4.0 | GUI + framebuffer + window manager | Planned |
| v0.5.0 | SMP + IPC + signals | Planned |
| v1.0.0 | Complete OS with all major subsystems | Future |

---

## Design Principles

1. **Correctness first** — never ship broken subsystems
2. **Build it before you document it** — verify with QEMU
3. **No fake features** — if it's not implemented, don't claim it works
4. **Educational clarity** — code should be readable while single-stepping in GDB
5. **Incremental improvement** — add one subsystem at a time, verify, document
