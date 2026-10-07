# VibeCagOS

> A hobby 32-bit x86 operating system built with architectural clarity,  
> educational depth, and genuine end-to-end functionality.

i386 Protected Mode | GRUB Multiboot | QEMU Tested
>
> The shell (`sh`) is an **ordinary Ring-3 user process**. The kernel starts it
> as `init` and no longer contains an interactive shell on its boot path.

---

## What This Is

VibeCagOS is a real operating system kernel that:

- Boots via GRUB (Multiboot v1) on x86 hardware and QEMU
- Runs in **32-bit protected mode** with proper GDT, IDT, and TSS
- Features a **dynamic kernel heap** (`kmalloc` / `kfree` / `kcalloc` / `krealloc` / `kmalloc_aligned`) with coalescing
- Provides a **Virtual Filesystem (VFS)** layer abstracting storage devices and pseudo-filesystems
- Implements **VibeFS**, a hierarchical persistent disk filesystem with directories, subdirectories, inodes, and `.` / `..` traversal
- Mounts **procfs** at `/proc` offering dynamic kernel introspection (`/proc/version`, `/proc/uptime`, `/proc/meminfo`, `/proc/cpuinfo`)
- Has a **preemptive scheduler** driven by a 100 Hz PIT timer, scheduling **real ring 3 user processes** (private address spaces, validated syscalls)
- Handles **CPU exceptions** with detailed register dumps
- Supports **PS/2 keyboard** and serial console simultaneously
- Has **paging enabled** with a kernel identity map
- Supports an **RTL8139 NIC** with ARP, IPv4, and ICMP ping
- Shows a **rich colored interactive shell** with directory navigation, CWD prompt, and command history

This is not a toy. Every subsystem that claims to work has been built, booted, tested, and verified in QEMU.

---

## Quick Start

### Requirements (WSL2 Ubuntu or Linux)

```bash
sudo apt install clang lld qemu-system-x86 \
                 grub-pc-bin grub-common xorriso mtools
```

### Build and Run

```bash
# Build bootable ISO
make

# Create fresh disk image (2MB)
dd if=/dev/zero of=disk.img bs=1M count=2

# Run with serial console (headless)
make run

# Run with VGA window visible
make run-window

# Run automated integration test
make test
```

### Debug with GDB

```bash
# Terminal 1 — start with GDB stub
make debug

# Terminal 2 — connect
gdb kernel.elf -ex 'target remote :1234' -ex 'break kernel_main' -ex 'continue'
```

---

## User Mode (Ring 3)

User programs run in ring 3 with a private page directory. The kernel is
identity-mapped but supervisor-only (`src/kernel/vmm.c` refuses any `PAGE_USER`
mapping outside the user window, so kernel pages can never become user-readable).

```
0x00000000 - __free_ram_end   kernel identity map, supervisor-only, SHARED
0x10000000 (USER_BASE)        user image: text read-only, data/bss read-write
0x1FFFF000 - 0x20000000       user stack (4 pages); page below left unmapped
up to 0xC0000000 (USER_END)   reserved for user space (heap/mmap later)
```

Constants live in `src/kernel/kernel.h` (`USER_BASE`, `USER_STACK_TOP`,
`USER_END`), where the full layout is documented.

- **Syscalls** use `int 0x80` (DPL 3 gate): `eax` = number, `ebx/ecx/edx` = args, result in `eax`.
  Every pointer argument is validated by `src/kernel/usercopy.c`
  (`copy_from_user` / `copy_to_user` / `strncpy_from_user`) before the kernel
  touches it: range, wrap-around, per-page `PAGE_USER` and read/write permission
  are all checked, so a buffer may span pages safely and a bad pointer yields
  `-EFAULT` instead of a kernel panic.
- **Preemption**: the PIT timer (100 Hz, 3-tick quantum) switches away from ring 3
  code by swapping kernel stacks inside `schedule()`; the interrupted process
  resumes through its own saved trap frame and `iret`s back to the exact faulting
  instruction. Ticks that land while the kernel is inside a syscall do not
  preempt -- the kernel is deliberately non-reentrant at this stage.
- **Faults in ring 3** (#GP, #PF, ...) terminate only the offending process;
  kernel-mode faults remain fatal.
- Programs are built from `src/user/*.c` against `src/user/ulib.h` and the shared
  ABI in `src/abi/syscall.h`, linked by `src/user/user.ld` at `USER_BASE` as flat
  "VBIN" images, then embedded into the kernel by `src/kernel/userblob.s` and
  listed in `src/kernel/progs.c`.

```
utest <n>       run a Ring-3 self test (& = background)
ps              list processes with their kernel state
```

`make test-all` runs the deterministic QEMU suites: `ring3`, `syscall`,
`scheduler`, `usercopy`, `faults`, `stdio`, `fs`, `shell`, `proc`.

---

## Shell Commands

Once booted, you'll see the colored prompt `vcos:<cwd>$` (e.g. `vcos:/$ ` or `vcos:/home/user$ `). Available commands:

| Command | Category | Description |
|---|---|---|
| `help` | General | Show all commands |
| `pwd` | Filesystem | Print current working directory |
| `cd <path>` | Filesystem | Change current working directory |
| `ls [path]` | Filesystem | List directory contents |
| `ls -l [path]` | Filesystem | Detailed file listing with sizes and types |
| `mkdir <path>` | Filesystem | Create a directory |
| `mkdir -p <path>` | Filesystem | Create directory tree recursively |
| `touch <file>` | Filesystem | Create empty file |
| `cat <file>` | Filesystem | Display file contents |
| `write <file>` | Filesystem | Write content to file interactively |
| `rm <file>` | Filesystem | Delete regular file |
| `rmdir <dir>` | Filesystem | Remove empty directory |
| `mv <src> <dst>` | Filesystem | Rename or move file / directory |
| `stat <path>` | Filesystem | Inspect file metadata (inode, size, mode, links) |
| `mounts` | System | Display active VFS mount table |
| `fsinfo` | System | Display VibeFS superblock and block allocation info |
| `heap` | System | Display kernel dynamic heap allocator stats |
| `mem` | System | Physical memory and heap statistics |
| `ps` | System | List running processes |
| `uname` | System | OS name, version, architecture, and subsystem info |
| `uptime` | System | System uptime (ticks and milliseconds) |
| `ping <ip>` | Network | Ping an IP via ICMP (RTL8139 NIC) |
| `format` | Admin | Reformat root VibeFS filesystem |
| `clear` | UI | Clear VGA screen |
| `hello` | General | Print greeting |
| `exit` | System | Sync filesystems and halt / ACPI power off |

**Shell features:**
- Up/down arrow keys for command history (last 16 commands)
- Backspace and in-line editing
- Colored prompt with active working directory
- Error messages in distinct colored text

---

## Architecture Overview

```
+--------------------------------------------------+
|              USER SPACE (Ring 3)                 |
|  Future userspace applications                   |
+--------------------------------------------------+
|              SYSCALL BOUNDARY (int 0x80)         |
+--------------------------------------------------+
|              KERNEL (Ring 0)                     |
|                                                  |
|  Interactive Shell (CWD, history, colored prompt)|
|  VFS (Mount table, vnodes, file descriptor table)|
|     ├── VibeFS (Hierarchical disk filesystem)    |
|     └── procfs (Dynamic /proc introspection)     |
|                                                  |
|  Preemptive Scheduler     Dynamic Heap (kmalloc) |
|  Paging (Identity map)    Physical Allocator     |
|  GDT / TSS                IDT (Exceptions, IRQs) |
|  PIT 100Hz Timer          PS/2 Keyboard Driver   |
|                                                  |
|  DRIVERS: VGA text | IDE PIO | PCI | RTL8139     |
|  NETWORK: Ethernet | ARP | IPv4 | ICMP           |
+--------------------------------------------------+
|              GRUB Multiboot v1                   |
+--------------------------------------------------+
```

---

## Source Structure

```
VibeCagOS/
├── Makefile               # Build system (make, run, debug, test)
├── README.md              # This file
├── docs/
│   ├── ARCHITECTURE_AUDIT.md  # Subsystem analysis
│   ├── BASELINE.md            # Verified boot baseline
│   ├── ROADMAP.md             # Feature roadmap
│   └── FINAL_STATUS.md        # Feature status table
├── src/
│   ├── kernel/
│   │   ├── boot.s         # Multiboot entry point -> kernel_main()
│   │   ├── interrupts.s   # ISR stubs (exceptions, IRQs, syscall)
│   │   ├── kernel.c       # Main kernel: GDT, IDT, PIC, PIT, KB, scheduler, shell
│   │   ├── kernel.h       # Kernel types, GDT/IDT/TSS structs, API
│   │   ├── kernel.ld      # Linker script (memory layout)
│   │   ├── kmalloc.c/h    # Dynamic kernel heap allocator
│   │   ├── common.c       # String functions, printf
│   │   └── common.h       # Types, constants, string API
│   ├── drivers/
│   │   ├── vga.c/h        # VGA text mode (80x25, 16 colors, cursor)
│   │   ├── ide.c/h        # IDE/ATA PIO disk driver
│   │   ├── pci.c/h        # PCI bus scanner
│   │   └── rtl8139.c/h    # RTL8139 Ethernet driver
│   ├── fs/
│   │   ├── vfs.c/h        # Virtual Filesystem abstraction layer
│   │   ├── vibefs.c/h     # Hierarchical disk filesystem
│   │   ├── procfs.c/h     # Dynamic pseudo-filesystem (/proc)
│   │   └── simplefs.c/h   # Legacy flat filesystem
│   └── net/
│       ├── ethernet.c/h   # Ethernet frame handling
│       ├── arp.c/h        # ARP resolution
│       ├── ipv4.c/h       # IPv4 packet handling
│       ├── icmp.c/h       # ICMP ping
│       └── netconfig.h    # Network configuration constants
```

---

## What Was Added in v0.3.0

1. **Kernel Dynamic Memory Allocator (`kmalloc` / `kfree`):**
   - First-fit allocator with boundary tags and free block coalescing.
   - Full support for `kmalloc`, `kfree`, `kcalloc`, `krealloc`, and `kmalloc_aligned`.
   - Complete heap tracking and statistics (`kmalloc_get_stats`, `heap` command).

2. **Virtual Filesystem (VFS) Layer:**
   - Unified interface with `struct vnode`, `struct file`, and `struct fs_ops`.
   - Mount table supporting multiple filesystems simultaneously (`/` and `/proc`).
   - CWD tracking and canonical path normalization (`.`, `..`, redundant slashes).

3. **VibeFS Hierarchical Filesystem:**
   - Inode-based disk filesystem supporting nested directories (`/home/user`, `/var/log`, `/usr/bin`, etc.).
   - Multi-block directory entries with `.` and `..` support.
   - Max file size up to 8KB (16 direct blocks × 512 bytes).
   - Inode sector flushing and block bitmap allocator.

4. **procfs Pseudo-Filesystem:**
   - Mounted at `/proc` with read callbacks.
   - Dynamic entries: `/proc/version`, `/proc/uptime`, `/proc/meminfo`, `/proc/cpuinfo`, `/proc/cmdline`, `/proc/mounts`.

5. **Shell Modernization & Directory Navigation:**
   - `cd`, `pwd`, `ls`, `ls -l`, `mkdir`, `mkdir -p`, `touch`, `rm`, `rmdir`, `mv`, `stat`.
   - Prompt reflects current working directory (`vcos:<cwd>$`).
   - Subsystem commands: `mounts`, `fsinfo`, `heap`.

6. **Stability & Bug Fixes:**
   - Fixed stack corruption bug in `vfs_lookup_parent` where parent component pointers referenced stack frames.
   - Fixed compiler type declarations for freestanding environment (`uintptr_t`, `intptr_t`).
   - Added ACPI poweroff ports (`0x604` and `0xB004`) for instantaneous, clean virtual machine shutdown.
   - Automated integration test target (`make test`) validating end-to-end execution.
