# VibeCagOS — Regression Matrix

> Every row is a subsystem that must keep working across the migration. `Test`
> is the automated suite that guards it (`make test-<name>`); `Known
> limitation` is honest about what is still missing. If a change breaks a row,
> fix it before moving on — no silent disappearance of working features.

| Subsystem | Baseline (pre-migration) | Current | Test | Known limitation |
|---|---|---|---|---|
| GRUB Multiboot boot | works | works | all suites boot | BIOS only, no UEFI |
| VGA text + serial console | works | works | all suites (serial log) | text mode only, no framebuffer |
| GDT / TSS / IDT | GRUB's GDT, IDT syscall-only | full GDT+TSS, DPL3 0x80 | `ring3`, `faults` | single TSS, single CPU |
| PIC / PIT 100 Hz | masked | remapped, preemptive tick | `scheduler` | — |
| PS/2 keyboard | serial-only polling | IRQ1 ring buffer, blocking read | `stdio`, `shell` | raw TTY, editing in user space |
| PS/2 mouse | polling demo | IRQ12 packets | none (manual `gui`) | no automated test |
| PMM bitmap allocator | bump allocator | bitmap, zeroed frames | `proc` (FramesFree leak check) | fixed 64 MiB pool, no discovery |
| kmalloc/kfree heap | none | first-fit + coalesce | `shell` (`heap` via procfs) | no stress test yet |
| Paging / VMM | off | per-process dirs, `CR0.WP` | `faults`, `usercopy` | no COW, no demand paging, no NX |
| Scheduler (round-robin) | dead code | preemptive, 30 ms quantum | `scheduler` | kernel non-preemptible by design |
| sleep / block / wakeup | none | tick deadlines, wait channels | `syscall`, `stdio`, `pipe` | — |
| Ring-3 processes | none | CPL3, private stacks/stacks | `ring3` | — |
| `int 0x80` + usercopy | dead code | 28 syscalls, per-page validation | `syscall`, `usercopy` | — |
| Per-process fds + TTY | none | 0/1/2 console, `OPEN_MAX` 16 | `stdio`, `fs` | no `dup2` (spawn-time inherit) |
| VFS | none | mounts, vnodes, refcounts | `fs`, `shell` | no locking (single CPU) |
| VibeFS | flat SimpleFS | inodes, dirs, 60 KiB max | `fs`, `shell` | no perms check, no indirect blocks |
| procfs | none | 11 entries | `shell` | static `/proc/net` |
| devfs | none | null/zero/console/random/tty | none direct (via console) | no automated test |
| IDE/ATA PIO | works | works (unchanged) | `fs`, `shell` (persist) | PIO, no DMA/IRQ |
| PCI scan | works | works (unchanged) | `shell` (`pci`) | — |
| RTL8139 / ARP / IPv4 / ICMP | works | works (unchanged) | none automated | polling NIC, QEMU lossy ping |
| UDP / DNS | works | works (unchanged) | none automated | kernel-shell commands only, no socket syscalls |
| ACPI poweroff | works | works (kinit path) | every suite ends with `exit` | — |
| ELF loader + spawn/exec | none (VBIN idea) | ELF32, FS-first + embedded fallback | `exec`, `proc` | 512 KiB image cap, `PF_X` unenforced |
| Pipes | none | blocking byte stream, EOF | `pipe` | no `O_NONBLOCK` |
| Shell (Ring-3) | Ring-0 `run_shell` | `/bin/sh` + `/sbin/init`, syscalls only | `shell`, `pipe` | some builtins remain (see audit #4) |
| `ls` / `cat` / `echo` | shell builtins | separate `/bin` ELFs | `pipe`, `exec` | — |
| GUI desktop | kernel Mode-13h demo | unchanged demo | none automated | still Ring 0 (milestone 15) |

## How to read a failure

1. `make test-all` names the suite; the kept log is `/tmp/vibe-last.log`.
2. Find the first `FAIL` line — later failures in the same suite are usually
   fallout (e.g. a hung `wait` starves every later prompt).
3. Pid-sensitive expects (`proc` suite) assume the layout `0 idle, 1 kinit,
   2 init, 3 sh`; if boot gains a process, update the pids deliberately, not
   by weakening the patterns.
