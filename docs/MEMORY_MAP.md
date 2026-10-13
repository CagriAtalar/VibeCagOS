# VibeCagOS — Memory Map

> Source of truth: `src/kernel/kernel.ld`, `src/kernel/kernel.h`,
> `src/kernel/pmm.c`, `src/kernel/vmm.c`. Kept in sync by hand; if the code
> disagrees with this file, the code wins and this file is a bug.

32-bit, two-level paging, **no PAE**. Page size 4 KiB. Kernel stays low and
identity-mapped (ADR-005).

## Virtual address space (per process)

```
0xFFFFFFFF ┌───────────────────────────────────────────────┐
           │ kernel-only virtual range                      │
0xC0000000 ├───────────────────────────────────────────────┤  ← MMIO (NIC BAR,
           │ .. unused user range (heap / mmap later) ..     │    LFB) mapped by
           │                                                │    the kernel only
0x20000000 ├───────────────────────────────────────────────┤
           │ user stack, 4 pages, grows down                │
0x1FFFC000 ├───────────────────────────────────────────────┤  ← USER_STACK_TOP
           │ .. gap ..                                       │
0x10000010*├───────────────────────────────────────────────┤
           │ user image: text RO @ base, data/bss RW after  │  ← USER_BASE
0x10000000 ├───────────────────────────────────────────────┤
           │ .. gap ..                                       │
 __free_ram_end ├──────────────────────────────────────────┤
           │ kernel image + boot stack + PMM frame window    │
           │ identity mapped, supervisor-only, SHARED by     │
0x00000000 │ every address space (PD slots < 64)             │
           └───────────────────────────────────────────────┘
      (* actual image end depends on the linked program size)
```

Page-directory **slots 0–63** (0x00000000–0x0FFFFFFF) and **slots ≥ 768**
(≥ 0xC0000000) hold the shared kernel mappings; user mappings live in **slots
64–767** and are private per process. `vmm_create_address_space()` copies the
kernel slots from the master directory.

Constants (`src/kernel/kernel.h`):

| Name | Value | Meaning |
|---|---|---|
| `USER_BASE` | `0x10000000` | first user byte; RO text starts here |
| `USER_STACK_TOP` | `0x20000000` | exclusive top of the user stack |
| `USER_STACK_PAGES` | 4 | user stack size (16 KiB) |
| `USER_END` | `0xC0000000` | first non-user address |

## Physical memory

```
0x00000000  low memory: IVT/BDA/EBDA (not used directly)
0x00100000  kernel image          (_start)  .boot/.text/.rodata/.data/.bss
              + 128 KiB boot stack -> __stack_top
__free_ram  + 64 MiB PMM frame window      -> __free_ram_end
```

- The linker script reserves the 64 MiB region so the linker/GRUB place nothing
  there. `kernel_main` calls `pmm_init(__free_ram, __free_ram_end)`; the PMM
  bitmap tracks that window (4 KiB frames, zeroed on allocation). The Multiboot
  memory map is parsed for the reported total, but the frame window itself is
  the linker-reserved region — simple and deterministic under QEMU.
- Kernel virtual addresses for that region equal physical addresses
  (identity map).

## Address spaces and switching

- One master kernel page directory holds the shared kernel mappings.
- Each process gets a private page directory (`struct process.page_table`),
  created by `vmm_create_address_space()`.
- A kernel thread has `page_table == NULL` and runs on the master directory.
- `switch_context()` (asm) saves callee-saved registers + ESP, switches CR3 to
  the new process's directory (or the master for kernel threads), points
  `TSS.esp0` at that process's kernel stack top, and restores the new context.

## Per-process kernel stack

`struct process` embeds a 32 KiB kernel stack (`KERNEL_STACK`), 16-byte aligned.
While a process runs, `TSS.esp0` points at its top; a Ring-3 → Ring-0 transition
(`int 0x80`, IRQ, exception) lands there. The trap frame of the interrupted
context lives on this stack.

## Known constraints

- No NX bit (32-bit paging without PAE) — `PF_X` in `vbin` is recorded but not
  enforced.
- No guard page below kernel stacks yet; a canary check in the scheduler catches
  obvious overflow (`PANIC "kernel stack overflow"`).
- 16 concurrent processes (`PROCS_MAX`), each with a full 32 KiB kernel stack in
  BSS.
