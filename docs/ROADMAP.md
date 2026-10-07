# VibeCagOS — Roadmap

> **Status:** Ring-3 migration milestones 0–14 complete, plus a real Ring-3
> `/sbin/init` with filesystem-first program loading. Next up: the GUI
> foundation (milestone 15). See `docs/ARCHITECTURE_AUDIT.md` for the state of
> the code and `docs/RING3.md` for the CPU-level flows.

---

## Milestones (the plan this repository was migrated along)

| # | Milestone | State | How it is checked |
|---|---|---|---|
| 0 | Baseline build + boot | done | `make`, `make test` |
| 1 | Architecture split (kernel/mm/fs/drivers) | done | source layout |
| 2 | Correct user address spaces | done | `test-faults` |
| 3 | Enter Ring 3 | done | `test-ring3` |
| 4 | `int 0x80` dispatcher | done | `test-syscall` |
| 5 | Preemptive scheduler | done | `test-scheduler` |
| 6 | sleep / yield / wakeup | done | `test-scheduler`, `test-syscall` |
| 7 | User memory safety (usercopy) | done | `test-usercopy` |
| 8 | User I/O and the console TTY | done | `test-stdio` |
| 9 | Per-process fd tables | done | `test-stdio`, `test-fs` |
| 10 | Userspace shell | done | `test-shell` |
| 11 | spawn / wait / exec | done | `test-proc`, `test-exec` |
| 12 | VBIN loader + filesystem programs (consolidated from ELF32) | done | `test-exec` |
| 13 | Pipes (IPC primitive) | done | `test-pipe` |
| 14 | Userspace utilities | done | `ls`, `cat`, `echo` are /bin programs |
| 14b | Ring-3 init + disk-first loading | done | `/sbin/init` spawns `/bin/sh`; bare names try `/bin` first |
| 15 | GUI foundation (framebuffer, input, IPC) | **next** | — |
| 16 | Compositor / window manager / apps | not started | — |

## Next: milestone 15 — GUI foundation

Not the GUI itself, and deliberately not before Ring 3, syscalls, the scheduler
and the shell are all correct (they are). The foundation is three kernel
primitives and one user-space program:

1. **Framebuffer abstraction.** A mode-set path (VESA/BIOS `0x4F15` or a
   simpler `0xB800`-style text framebuffer first) plus
   `struct framebuffer { address, width, height, pitch, bpp }`. MMIO must be
   mapped through `vmm_map_mmio()` into supervisor-only pages — never into
   every process. Today `vmm_map_mmio()` already does the right thing; nothing
   calls it except MMIO setup.
2. **Input event queue.** IRQ1 (keyboard) and IRQ12 (mouse) already exist in
   the kernel and push bytes into a ring buffer. The GUI needs structured
   events (key with modifiers, mouse with coordinates and buttons), produced in
   the kernel and consumed by one user-space reader — not by each application.
   `pipe()` already provides the transport; a shared ring buffer in mapped
   memory is the cheaper option for high-rate mouse motion.
3. **A way for user space to see the framebuffer.** Either a single-purpose
   syscall that maps one framebuffer region into the calling process's address
   space, or a fixed, kernel-created shared mapping. Whichever it is, it must
   be a deliberate, audited mapping: not "identity map everything as user".
4. **A user-space display server** that owns the framebuffer, reads the input
   queue and draws. Compositor, window manager and applications run inside it.

The kernel must keep only the primitives. `src/drivers/gui.c` (a ring-0 demo
desktop) is legacy and should be deleted once the user-space server works.

## After that

- **More utilities as programs**: `mkdir`, `rm`, `mv`, `cp`, `stat`, `ps`,
  `head`, `hexdump` are still shell builtins; each is a `/bin` program now that
  fd inheritance makes redirection work for them too.
- **Stop embedding binaries in the kernel** (defect 3 in the audit): build a
  populated `disk.img` so `/bin` is the only source of programs.
- **Security hardening**: honour `mode` in VibeFS, `NX` once PAE is on, per-user
  `mmap`, guard pages below the user stack.
- **SMP**: only after single-CPU scheduling is boring; the existing
  `run-smp` target boots a second CPU that nothing schedules.

## Design principles this repository follows

1. Correctness first — a milestone is not done because it boots, it is done
   because a test asserts the property and passes.
2. Build it, then document it — every status above is backed by `make test-all`.
3. No fake features — if it is not implemented and tested, it is marked as such.
4. Educational clarity — the interesting decisions are in comments, not hidden
   behind abstractions.
5. One change at a time, with tests, so a regression is attributable.