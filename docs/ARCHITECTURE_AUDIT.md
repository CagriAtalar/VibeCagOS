# VibeCagOS — Architecture Audit

> **Audit date:** 2026-10-02 | **Boot-tested in QEMU** | **Build verified**

---

## 1. Current Boot Flow

```
BIOS firmware
    |
    v
GRUB (Multiboot v1) - loads kernel.elf at 0x100000 (1 MiB)
    |
    v
boot.s _start
    |  cli, set esp=__stack_top, clear EFLAGS
    v
kernel_main()
    +-- memset __bss, 0
    +-- vga_init()      (0xB8000 text buffer, 80x25)
    +-- serial_init()   (COM1 0x3F8)
    +-- idt_init()      (256-entry IDT, only int 0x80 installed)
    +-- pic_init()      (8259 PIC remapped 0x20/0x28, ALL IRQs masked)
    +-- sti()
    +-- ide_init()      (ATA PIO polling)
    +-- pci_init()      (scans all 256 buses for RTL8139)
    +-- nic_init()      (RTL8139 setup)
    +-- simplefs_mount() or simplefs_format()
    v
Interactive shell (busy-loop polling COM1)
```

**Verified QEMU output:**
```
x86 OS - SimpleFS (IDE Disk)
=====================================
IDE disk driver initialized
IDE drive detected and ready
PCI: Found RTL8139 at 0:3.0  IO=0x0000c000  IRQ=11
RTL8139: MAC xx:xx:xx:xx:xx:xx  NIC ready
Mounting SimpleFS...
>
```

---

## 2. Current Memory Map

| Region | Physical Address | Notes |
|---|---|---|
| NULL / BIOS | 0x00000000–0x000FFFFF | Low 1 MiB |
| Kernel image | 0x00100000 | Hard-coded in kernel.ld |
| .boot / .text | 0x00100000+ | Multiboot header first |
| .rodata | After .text | |
| .data | After .rodata | |
| .bss | After .data | Cleared by kernel_main |
| Kernel stack | bss_end + 128 KiB | Embedded in ELF |
| Free RAM pool | Stack + 4096 align | 64 MiB bump allocator |
| VGA text buffer | 0x000B8000 | Direct identity mapped |

**No paging enabled. Virtual == Physical everywhere.**

---

## 3. Current Virtual Address Map

Paging is **disabled**. `create_process()` builds 2-level x86 page tables but `enable_paging()` / `load_cr3()` are never called from `kernel_main()`.

---

## 4. Current Interrupt Flow

```
int 0x80 (ONLY vector configured in IDT)
    |
    v
isr128 (interrupts.s)
    | push $0 (dummy error code), push $128 (interrupt number)
    v
isr_common
    | push eax, ecx, edx, ebx, esp, ebp, esi, edi
    v
handle_interrupt(struct trap_frame *)
    |
    +-- int_no == 128 -> handle_syscall(f)
    +-- else -> PANIC

No timer, no keyboard, no disk interrupts. ALL PIC IRQs masked.
```

---

## 5. Current Syscall Flow

System call numbers defined in `common.h` — mostly **unimplemented**:

| Number | Name | Status |
|---|---|---|
| 1 | SYS_PUTCHAR | Implemented |
| 2 | SYS_GETCHAR | Implemented (busy-loop) |
| 3 | SYS_EXIT | Implemented (dead code) |
| 4 | SYS_READFILE | Defined, NO HANDLER |
| 5 | SYS_WRITEFILE | Defined, NO HANDLER |
| 6–10 | SYS_ADDFILE, etc | Defined, NO HANDLER |

All syscall code is **dead code** — no user processes exist.

---

## 6. Current Process Creation Flow

`create_process(image, size)` exists but is **never called**:

1. Find free slot in `procs[8]`
2. Set up kernel stack with `user_entry` as return address
3. Allocate page directory (via `alloc_pages(1)`)
4. Map kernel identity (`__kernel_base`–`__free_ram_end`)
5. Copy image to user pages at `USER_BASE = 0x1000000`
6. Mark `PROC_RUNNABLE`

Since paging is disabled and `create_process` is never invoked, no processes run.

---

## 7. Current Context-Switch Flow

`switch_context(prev_sp, next_sp)` and `yield()` exist as **dead code**:

```asm
push ebp, ebx, esi, edi
mov [eax], esp     ; save prev->sp
mov esp, [edx]     ; load next->sp
pop edi, esi, ebx, ebp
ret                ; jump to new task's saved return address
```

---

## 8. Current Filesystem Flow

```
Shell: "create foo.txt"
    |
    v
simplefs_create("foo.txt")
    | find_free_inode() - O(n) linear scan
    | init inode in memory
    | read_write_disk(sector, write=1)
    v
ide_write_sector(lba, buf)
    | outb/inw PIO ATA
    v
ATA disk hardware
```

**Disk layout:**
- Sector 0: Superblock
- Sectors 1–10: Inode table (64 inodes)
- Sectors 11+: Data blocks (first-fit, O(n) search per write)

---

## 9. Current Input Flow

```
COM1 serial port (0x3F8) — POLLING ONLY
    | busy-loop: while (inb(COM1+5) & 1) == 0) ;
    v
getchar() returns char or -1
    |
    v
Shell loop in kernel_main (while(1))
```

No PS/2 keyboard driver. No interrupt-driven input.

---

## 10. Current Output / Rendering Flow

```
putchar(ch)
    |
    +-- wait for COM1 TX ready (busy-loop)
    +-- outb(COM1, ch) -> serial
    +-- vga_putchar(ch) -> 0xB8000 text buffer (80x25, white-on-black)
```

No framebuffer. No colors. No graphics. No GUI.

---

## 11. Current Build Flow

```
make
    | clang -m32 *.s -> *.o
    | clang -m32 *.c -> *.o (9 translation units)
    | clang -Wl,-T kernel.ld -> kernel.elf
    v
grub-mkrescue -> os.iso

make run:
    qemu-system-i386 -cdrom os.iso -hda disk.img -serial stdio
    -m 128M -display none -netdev user,id=n0 -device rtl8139,netdev=n0
```

Build works on WSL2 Ubuntu with clang-18, lld, grub-pc-bin, qemu-system-i386.

---

## 12. Known Limitations

| # | Limitation | Severity |
|---|---|---|
| 1 | Paging never enabled | Critical |
| 2 | No userspace — everything ring 0 | Critical |
| 3 | No timer interrupt (IRQ0 masked) | High |
| 4 | No keyboard driver (only serial) | High |
| 5 | No framebuffer/graphics (80x25 VGA text) | High |
| 6 | First file write bug (README acknowledges) | Medium |
| 7 | Max file size 2 KB (4 block pointers) | Medium |
| 8 | No kfree() — bump allocator only | Medium |
| 9 | No GDT/TSS setup | High |
| 10 | Duplicate inb/outb in ide.c and kernel.h | Low |
| 11 | All PIC IRQs masked (0xFF/0xFF) | High |
| 12 | No ACPI, no time source | Medium |
| 13 | No path handling — flat namespace | Medium |
| 14 | Hard-coded 64 MiB RAM in linker | Medium |

---

## 13. Known Correctness Problems

**Critical:**
- `create_process()` builds page tables that are never activated (paging off)
- No TSS — ring 3 iret would triple-fault immediately
- ISR: `popl %esp` in isr_common is architecturally problematic
- `strcpy(inode->filename, filename)` — no 56-byte bounds check

**High:**
- Linker embeds 64 MiB RAM pool in ELF BSS (huge binary / slow load)
- `alloc_block()` is O(n^2) — rescans all inodes per write
- First file write bug (off-by-one in block pointer init)
- `SYS_GETCHAR` busy-loops consuming 100% CPU while waiting

---

## 14. Proposed Architecture

```
+--------------------------------------------------+
|           USER SPACE (Ring 3)                    |
|  init | shell | ps | ls | cat | GUI apps         |
+--------------------------------------------------+
|           SYSCALL BOUNDARY (int 0x80)            |
+--------------------------------------------------+
|           KERNEL (Ring 0)                        |
|                                                  |
|  TTY/shell    VFS/SimpleFS   Scheduler           |
|  VMM (virtual memory)  PMM (frame allocator)     |
|  IDT/GDT/TSS  PIT timer  PS/2 keyboard           |
|                                                  |
|  DRIVERS: VGA | IDE | PCI | RTL8139             |
+--------------------------------------------------+
|           GRUB Multiboot (retained)              |
+--------------------------------------------------+
```

---

## 15. Migration Plan

**Phase 1 — Foundation**
- Proper GDT: null, kernel code/data, user code/data, TSS
- TSS for ring3→ring0 kernel stack
- Enable paging (identity map)
- PIT IRQ0: preemptive scheduler tick
- PS/2 IRQ1: scancode→ASCII keyboard driver
- Informative panic screen with register dump

**Phase 2 — Memory Management**
- Bitmap physical frame allocator
- kmalloc/kfree kernel heap
- VM manager (map/unmap/protect)

**Phase 3 — Scheduler and Processes**
- Preemptive round-robin scheduler
- RUNNING/READY/SLEEPING/ZOMBIE states
- sleep/wake/yield/exit/wait

**Phase 4 — Userspace**
- Ring 3 process launch
- Syscall table with validation
- copy_from_user / copy_to_user

**Phase 5 — Shell Improvements**
- Command history, arrow keys
- Color VGA output
- ps, mem, uptime, uname, clear

**Phase 6 — Filesystem Fixes**
- Fix first-file write bug
- Larger file support
- Better block allocator

**Phase 7 — Documentation and Testing**
- Complete docs/ directory
- Host unit tests
- QEMU integration tests
