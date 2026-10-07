# VibeCagOS — Roadmap

> **Status:** Ring-3 migration milestones 0–14 complete. ELF32 loader and
> `exec()` landed most recently. Next up: userspace utilities, then the GUI
> foundation. See `docs/ARCHITECTURE_AUDIT.md` for the state of the code and
> `docs/RING3.md` for the CPU-level flows.

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
| 12 | ELF32 loader + filesystem programs | done | `test-exec` |
| 13 | Pipes (IPC primitive) | done | `test-pipe` |
| 14 | Userspace utilities | **next** | — |
| 15 | GUI foundation (framebuffer, input, IPC) | not started | — |
| 16 | Compositor / window manager / apps | not started | — |

## Next: milestone 14 — utilities as real user programs

The shell currently implements `ls`, `cat`, `echo`, `mkdir`, `stat`, `ps`, … as
builtins. Each should become an ELF program in `/bin`, loaded by the same ELF
loader, with `sh` only handling dispatch, redirection and `exec`.

Why, and what it proves: it removes the last sizable chunk of shell logic from
Ring 3 builtins, exercises the VFS + ELF path for every command, and makes the
"no kernel header in user code" boundary hold for real programs rather than for
one hand-written shell.

Blocks it needs first:

- **fd inheritance in `spawn`** — a child needs to be able to run with a
  redirected stdout (`ls > out.txt`) or a pipe. Today `spawn` always gives the
  child a fresh console for fds 0/1/2, which is also why the shell cannot do
  `ls | grep`. This is the natural next kernel change: pass a small fd map to
  `spawn`, and let `sh` build the map for `>` and `|`.
- **build system**: the Makefile's `USER_PROGS` list already drives
  `userblob.s`, so adding utilities is a list edit, but they should also stop
  being embedded once `/bin` is populated (defect 3 in the audit).

## After that

- **GUI foundation (milestone 15)**: framebuffer abstraction
  (`struct framebuffer { address, width, height, pitch, bpp }`), an input event
  queue fed by the existing IRQ1/IRQ12 drivers, and shared memory so a
  *user-space* display server can own the framebuffer. The kernel exposes
  primitives only; no GUI toolkit in ring 0.
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