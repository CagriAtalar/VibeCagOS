# VibeCagOS

> A hobby 32-bit x86 operating system built with architectural clarity,  
> educational depth, and genuine end-to-end functionality.

**Version 0.2.0** | i386 Protected Mode | GRUB Multiboot | QEMU Tested

---

## What This Is

VibeCagOS is a real operating system kernel that:

- Boots via GRUB (Multiboot v1) on x86 hardware and QEMU
- Runs in **32-bit protected mode** with proper GDT, IDT, and TSS
- Has a **preemptive scheduler** driven by a 100 Hz PIT timer
- Handles **CPU exceptions** with detailed register dumps
- Supports **PS/2 keyboard** and serial console simultaneously
- Has **paging enabled** with a kernel identity map
- Runs a **persistent flat filesystem** (SimpleFS) on IDE disk
- Supports an **RTL8139 NIC** with ARP, IPv4, and ICMP ping
- Shows a **colored interactive shell** with command history

This is not a toy. Every subsystem that claims to work has been verified in QEMU.

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

# Create fresh disk image
dd if=/dev/zero of=disk.img bs=1M count=2

# Run with serial console (headless)
make run

# Run with VGA window visible
make run-window
```

### Debug with GDB

```bash
# Terminal 1 — start with GDB stub
make debug

# Terminal 2 — connect
gdb kernel.elf -ex 'target remote :1234' -ex 'break kernel_main' -ex 'continue'
```

---

## Shell Commands

Once booted, you'll see the `vcos$` prompt. Available commands:

| Command | Description |
|---|---|
| `help` | Show all commands |
| `uname` | OS name, version, arch |
| `uptime` | System uptime |
| `mem` | Physical memory statistics |
| `ps` | List running processes |
| `ls` | List filesystem files |
| `cat <file>` | Display file contents |
| `create <file>` | Create new empty file |
| `write <file>` | Write content to file (line by line) |
| `rm <file>` | Delete file |
| `format` | Format filesystem (requires confirmation) |
| `ping <ip>` | Ping an IP via ICMP |
| `clear` | Clear screen |
| `hello` | Print greeting |
| `exit` | Halt system |

**Shell features:**
- Up/down arrow key for command history (last 16 commands)
- Backspace editing
- Colored prompt (`vcos$` in green)
- Error messages in red

---

## Architecture Overview

```
+--------------------------------------------------+
|              USER SPACE (Ring 3) — Planned       |
|  init | shell | apps                             |
+--------------------------------------------------+
|              SYSCALL BOUNDARY (int 0x80)         |
+--------------------------------------------------+
|              KERNEL (Ring 0)                     |
|                                                  |
|  Shell (interactive)  SimpleFS (IDE disk)        |
|  Preemptive Scheduler  Physical Allocator        |
|  Paging (identity map)  GDT / TSS               |
|  IDT (exceptions + IRQs + syscall)               |
|  PIT 100Hz  PS/2 Keyboard  Serial COM1           |
|                                                  |
|  DRIVERS: VGA text | IDE PIO | PCI | RTL8139    |
+--------------------------------------------------+
|              GRUB Multiboot v1                   |
+--------------------------------------------------+
```

**Memory layout:**

| Region | Physical Address |
|---|---|
| Kernel | 0x00100000 (1 MiB) |
| Kernel stack | ~0x14F000 (128 KiB) |
| Free RAM pool | ~0x150000 – 0x4150000 (64 MiB) |
| VGA text buffer | 0xB8000 |

---

## Source Structure

```
VibeCagOS/
├── Makefile               # Build system (make, run, debug, disassemble)
├── README.md              # This file
├── docs/
│   ├── ARCHITECTURE_AUDIT.md  # Full architectural analysis
│   ├── BASELINE.md            # Verified boot baseline
│   ├── ROADMAP.md             # Feature roadmap
│   └── FINAL_STATUS.md        # Current feature status table
├── src/
│   ├── kernel/
│   │   ├── boot.s         # Multiboot entry point → kernel_main()
│   │   ├── interrupts.s   # ISR stubs (exceptions, IRQs, syscall)
│   │   ├── kernel.c       # Main kernel: GDT, IDT, PIC, PIT, KB, scheduler, shell
│   │   ├── kernel.h       # Kernel types, GDT/IDT/TSS structs, API
│   │   ├── kernel.ld      # Linker script (memory layout)
│   │   ├── common.c       # String functions, printf
│   │   └── common.h       # Types, constants, string API
│   ├── drivers/
│   │   ├── vga.c/h        # VGA text mode (80x25, 16 colors, cursor)
│   │   ├── ide.c/h        # IDE/ATA PIO disk driver
│   │   ├── pci.c/h        # PCI bus scanner
│   │   └── rtl8139.c/h    # RTL8139 Ethernet driver
│   ├── fs/
│   │   ├── simplefs.c     # Inode-based flat filesystem
│   │   └── simplefs.h     # Filesystem API and constants
│   └── net/
│       ├── ethernet.c/h   # Ethernet frame handling
│       ├── arp.c/h        # ARP resolution
│       ├── ipv4.c/h       # IPv4 packet handling
│       ├── icmp.c/h       # ICMP ping
│       └── netconfig.h    # Network configuration constants
```

---

## What Was Fixed (v0.2.0)

### Critical Bug Fixes

1. **First-file write bug** (mentioned in original README)  
   Root cause: `flush_inode_table()` was iterating by inode index rather than by sector, causing the first sector's data to be written to the wrong disk location.  
   Fix: Rewrote to iterate sector by sector (`for s in 0..INODE_TBL_SECS: write sector 1+s`).

2. **Paging was disabled** — `enable_paging()` was never called.  
   Fix: Added `paging_init()` that sets up identity-mapped page tables and enables CR0.PG.

3. **No GDT setup** — kernel relied on GRUB's minimal GDT.  
   Fix: Full 6-entry GDT with TSS.

4. **All PIC IRQs masked** — no timer, no keyboard.  
   Fix: Unmask IRQ0 (timer) and IRQ1 (keyboard).

### Improvements

- Preemptive scheduler at 100 Hz
- PS/2 keyboard driver with scancode decoder
- VGA 16-color support
- Command history with arrow keys
- Informative panic screen with register dump
- Improved block allocator (O(n) bitmap vs O(n^2))
- Full printf with width/padding/flags
- Comprehensive documentation in `docs/`

---

## Known Limitations

- **No userspace** — everything runs at ring 0 (scheduled for v0.3.0)
- **No kfree()** — bump allocator only (scheduled for Phase 2)
- **Flat filesystem** — no directories (scheduled for Phase 5)
- **Max file size** — 4 KB per file (8 blocks × 512 bytes)
- **Single CPU** — no SMP support
- **No GUI** — VGA text mode only (planned Phase 8)

---

## VirtualBox Note

VirtualBox boot may have issues with timing. QEMU is the recommended
emulator. For VirtualBox, try enabling hardware acceleration and
using the PIIX3 chipset.

---

## Documentation

Full documentation in `docs/`:

- [`ARCHITECTURE_AUDIT.md`](docs/ARCHITECTURE_AUDIT.md) — complete analysis of all subsystems
- [`BASELINE.md`](docs/BASELINE.md) — verified boot output and configuration
- [`ROADMAP.md`](docs/ROADMAP.md) — planned features and milestones
- [`FINAL_STATUS.md`](docs/FINAL_STATUS.md) — feature-by-feature status table
