# Ring 3 execution model (VibeCagOS)

## Address space (32-bit, 2-level paging)
| Range | Use | Access |
|---|---|---|
| `0 .. __free_ram_end` (~65 MB) | kernel, identity mapped, page tables **shared** by all processes | supervisor only |
| `0x10000000` `USER_BASE` | user image: header+text+rodata (RO), data+bss (RW) | user |
| `0x1FFFC000 .. 0x20000000` | user stack, 4 pages, page below is an unmapped guard | user RW |
| `.. 0xC0000000` `USER_END` | reserved for user heap | user |
| `0xC0000000 ..` | kernel-only (MMIO) | supervisor |

`vmm_map_page()` silently strips `PAGE_USER` outside `[USER_BASE, USER_END)` (and logs an error),
so a kernel address can never become user-accessible. User page tables live in PDE slots 64..767,
which are private per process; all other PDEs are copied from the master directory.
CR0.WP is set so ring 0 also honours read-only PTEs.

## Trap frame (`struct trap_frame`, built by `isr_common`)
low -> high: `gs fs es ds | edi esi ebp esp(dummy) ebx edx ecx eax | int_no err_code | eip cs eflags | [user_esp user_ss]`.
The last two words exist only when `cs & 3 == 3` (privilege change). Use `TF_FROM_USER(f)`.

## Flows
**syscall**: `int 0x80` -> CPU loads `SS0:ESP0` from TSS -> `isr128` -> `isr_common` (saves frame, loads kernel DS)
-> `handle_interrupt` -> `syscall_dispatch(f)` -> `f->eax = ret` -> `trapret` -> `iret` -> Ring 3.

**timer preemption**: IRQ0 -> same entry path -> `handle_interrupt`: `ticks++`, EOI, `process_tick(f)`
(wakes sleepers; if `TF_FROM_USER(f)` and the quantum is used up -> `schedule_locked()`)
-> `switch_context()` swaps **kernel stacks** -> new process returns up *its own* call chain
-> `trapret` -> `iret` to the exact instruction where it was interrupted.

**why this is safe**: the interrupt frame is stored on the *per-process* kernel stack (TSS.esp0 is updated
on every switch). `switch_context` only swaps callee-saved registers and ESP; nothing of the old process
is left half-finished because its frame simply stays on its own stack until it is scheduled again.
A new process gets a hand-built frame + a fake `switch_context` frame returning into `trapret`.

**preemption rule**: kernel mode is not preemptible. The timer only switches when it interrupted Ring 3
(or the idle thread). Kernel threads/syscalls give up the CPU at `yield/block_on/sleep_ms`.

## User pointers
Never dereferenced by the kernel. `copy_from_user / copy_to_user / strncpy_from_user / user_range_valid`
(`usercopy.c`) validate range, wrap-around, PDE+PTE present, `USER` bit and `WRITABLE` (for writes) for
**every page**, before copying anything (no partial copies).

## Tests (`make test-all`)
`tests/run.sh <ring3|syscall|scheduler|usercopy|faults>` boots QEMU headless, drives the kernel shell over
serial (`utest <n>`, `utest2 <a> <b>`, `uspawn <n>`) and greps the serial log. Test programs: `src/user/utest.c`.
