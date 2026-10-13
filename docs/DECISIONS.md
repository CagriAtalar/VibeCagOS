# VibeCagOS — Decisions (ADR log)

Architecture Decision Records in the lightweight form: **Context → Decision →
Consequences**. Newest families are grouped; each record has a stable id. When
a decision changes, add a new record that supersedes the old one instead of
editing history.

---

## ADR-001 — The shell and applications are Ring-3 processes

**Context.** Early VibeCagOS ran an interactive shell inside the kernel (Ring 0)
and the "known state" handed to the project described everything as Ring 0.

**Decision.** The kernel does **not** own a shell. On boot it creates the idle
thread (pid 0) and a small kernel init thread, which spawns `/sbin/init`; `init`
spawns `/bin/sh`. Every command is an ordinary Ring-3 process. A legacy Ring-0
debug shell still exists but only when built with `make KSHELL=1`, and is off the
default boot path.

**Consequences.** Real privilege separation and a real syscall boundary. The
kernel can only act on user requests through `int 0x80`. Boot now depends on the
file/program subsystem being up.

---

## ADR-002 — VBIN is the single runtime executable format

**Context.** The project wants one authoritative loader and no competing paths.
The master plan suggested ELF executables loaded from an initrd.

**Decision.** Consolidate on **VBIN**, a 28-byte-header custom format: read-only
text at `USER_BASE`, read-write data page-aligned after it, BSS zeroed to
`mem_size`. User programs are compiled and linked as ELF32 (a host build
intermediate only) and converted by `tools/vbinpack`. The kernel has **no ELF
loader**. Fds 0/1/2 survive `exec`; everything else closes.

**Consequences.** A tiny, auditable loader with explicit RO/RW permissions and
no relocations at load time. Cost: VBIN images are not standard ELF and cannot
be run on a host; the host ELF is only a build artifact. A future initrd (if
added) is a *delivery* mechanism for VBIN files, not a format change.

---

## ADR-003 — `SYS_SPAWNFDS` stands in for `fork` + `dup2`

**Context.** Pipes and redirection need fd plumbing, but `fork`/`dup2` did not
exist yet.

**Decision.** `SYS_SPAWNFDS(name, argv, in, out, err)` creates a child whose fds
0/1/2 are taken from the caller's own descriptors (`-1` = closed). The shell
uses this to hand a pipeline or redirection to a child.

**Consequences.** Pipelines and `>`/`>>`/`<` work today without a fork. This is
temporary: `fork` + `dup2` are planned (Step 1) and will supersede it for the
common case. Recorded so the "one authoritative mechanism" rule is not violated
silently during the transition.

---

## ADR-004 — VibeFS is the disk filesystem

**Context.** The master plan suggested FAT32 (host-inspectable, `fsck`-able) or
an ext2-like design.

**Decision.** Keep **VibeFS**: 512-byte sectors, one inode per sector, 118
direct + single + double indirect blocks (~8 MiB files), persistent on an IDE
disk. It mounts at `/`, procfs at `/proc`, devfs at `/dev`.

**Consequences.** The FS already works, persists across reboots (`test-persist`)
and is covered by tests. Cost: it is custom, so `fsck.fat`/host tools cannot
inspect `disk.img`, and there is no journaling. FAT32 may be added later as a
*second* filesystem to demonstrate the VFS's mount table, not as a replacement.

---

## ADR-005 — Kernel stays low, identity-mapped

**Context.** The master plan suggested a higher-half kernel mapping.

**Decision.** Keep the kernel at `0x100000`, identity-mapped and supervisor-only
in every address space. It is not moved to the higher half.

**Consequences.** Simple and correct for i386 without PAE; the kernel is
reachable at fixed low addresses and page tables are shared across processes.
A higher-half split is a broad refactor with no functional payoff here, so it is
deliberately out of scope. Documented in `docs/MEMORY_MAP.md`.

---

## ADR-006 — Keep the current `src/` layout

**Context.** The master plan sketches `boot/`, `kernel/arch/<arch>/`, `mm/`,
`lib/`.

**Decision.** Keep `src/{kernel,drivers,fs,net,user,abi}`. Do not restructure
for its own sake.

**Consequences.** Avoids churning every build rule and include path with no
behavioural change. Architecture-specific code is *not* yet isolated under
`arch/`; that split will be introduced only if/when a second architecture is
actually attempted.

---

## ADR-007 — Warnings are errors

**Decision.** The kernel and user builds compile with `-Werror` (on top of
`-Wall -Wextra`) and `-MMD -MP` automatic dependencies.

**Consequences.** No warning can land silently and header changes always trigger
the right rebuilds. Cost: contributors must fix warnings before committing.

---

## ADR-008 — `init` is pid 1

**Context.** The kernel's `kinit` thread is currently pid 1 and `/sbin/init` is
pid 2.

**Decision.** Make the first user process (`init`) pid 1 and keep kernel threads
out of the user pid namespace. `init` becomes the reaper and the orphan
reparent target.

**Consequences.** Matches Unix convention and the master plan; orphan handling
moves from the idle thread to `init`. Requires updating pid-sensitive tests.
Planned in Step 1.

---

## ADR-009 — Syscalls via `int 0x80`

**Decision.** Use `int 0x80` as the syscall instruction (not `sysenter`), with
`EAX` = number, `EBX/ECX/EDX/ESI/EDI` = args, return in `EAX`, negative = errno.

**Consequences.** Simple and portable across 32-bit x86; only vector 0x80 is
reachable from Ring 3 (all other IDT entries are DPL 0). `sysenter` can be added
later as a fast path without changing the ABI.

---

## ADR-010 — Automated testing via headless QEMU + serial assertions

**Decision.** Tests boot QEMU headless, drive the **user shell** over the serial
port, and assert on the captured serial log with per-suite expectations. The
persistence suite boots twice on one disk image. A `isa-debug-exit` based exit
code and a kernel `KTEST` harness are being added so runs also report a real
exit status.

**Consequences.** Tests exercise the real user-visible path (shell → programs →
syscalls) rather than kernel-internal entry points. Kept deterministic by
waiting for prompts instead of fixed sleeps. See `docs/TESTING.md`.
