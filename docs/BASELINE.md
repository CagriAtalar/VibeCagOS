# VibeCagOS — Baseline

> **Date:** 2026-10-02 | **Verified by:** Actual build and QEMU boot

---

## Build Command

```bash
# In WSL2 Ubuntu with clang-18, lld, grub-pc-bin, qemu-system-i386
make

# Create fresh disk image
dd if=/dev/zero of=disk.img bs=1M count=2
```

## Run Command

```bash
# Serial console only (no window)
make run
# Equivalent:
qemu-system-i386 -cdrom os.iso -hda disk.img -serial stdio \
    -no-reboot -m 128M -display none \
    -netdev user,id=n0 -device rtl8139,netdev=n0

# With QEMU window (VGA output)
make run-window
```

## QEMU Arguments

| Argument | Purpose |
|---|---|
| `-cdrom os.iso` | Boot from GRUB ISO |
| `-hda disk.img` | 2 MiB IDE disk for SimpleFS |
| `-serial stdio` | Serial console on stdin/stdout |
| `-no-reboot` | Halt instead of reboot on error |
| `-m 128M` | 128 MiB RAM |
| `-display none` | Headless (serial only) |
| `-netdev user,id=n0` | QEMU user-mode networking |
| `-device rtl8139,netdev=n0` | Virtual RTL8139 NIC |

## Target Architecture

- **CPU:** i386 (32-bit x86 protected mode)
- **Compiler:** clang -m32
- **Linker:** LLD (via `-fuse-ld=lld`)
- **Boot Protocol:** GRUB Multiboot v1
- **Boot Mode:** BIOS only (no UEFI)

## Current Memory Assumptions

- Kernel loads at physical address 0x100000 (1 MiB, hard-coded in kernel.ld)
- Free RAM pool: 64 MiB starting after kernel stack (hard-coded)
- No physical memory discovery — assumes QEMU provides 128 MiB
- VGA text buffer at 0xB8000 (assumed identity-mapped)
- Serial port at 0x3F8 (COM1, hard-coded)

## Actual Verified Boot Output

```
x86 OS - SimpleFS (IDE Disk)
=====================================
Input: Serial Console (QEMU)
Output: VGA + Serial Console
IDE disk driver initialized
IDE drive detected and ready
PCI: Scanning bus...
PCI: Found RTL8139 at 0:3.0  IO=0x0000c000  IRQ=11
RTL8139: MAC 82:84:0:18:52:86  IO=0x0000c000  NIC ready

Mounting SimpleFS...
Invalid filesystem! Please format first.
Formatting new filesystem...
Formatting disk with SimpleFS...
Filesystem formatted successfully!
  Total blocks: 4096
  Inode blocks: 10
  Data blocks: 1024
  Max files: 64

>
```

## Currently Working Features

- [x] GRUB Multiboot boot
- [x] Protected mode (32-bit)
- [x] VGA text mode output (80x25)
- [x] Serial console output (COM1)
- [x] Serial console input (polling)
- [x] IDE disk driver (PIO mode)
- [x] PCI bus scan
- [x] RTL8139 NIC initialization (polling)
- [x] SimpleFS format and mount
- [x] File create
- [x] File list (ls)
- [x] File delete (rm)
- [x] File read (cat)
- [x] File write (write) — NOTE: first file may have bug
- [x] ARP resolution
- [x] ICMP ping (basic)
- [x] Shell with backspace support

## Currently Broken / Missing Features

- [ ] **Paging disabled** — `enable_paging()` never called
- [ ] **No keyboard driver** — only serial input
- [ ] **No timer interrupt** — PIC IRQs all masked
- [ ] **No userspace** — everything at ring 0
- [ ] **No process scheduler** — create_process/yield are dead code
- [ ] **No GDT/TSS** — relies on GRUB's minimal GDT
- [ ] **First file write bug** — acknowledged in README
- [ ] **VirtualBox boot broken** — loops without input (README)
- [ ] **No graphics/framebuffer**
- [ ] **No command history**
- [ ] **No tab completion**
- [ ] **No colors in terminal**
- [ ] **Max file size 2 KB**

## Current Boot Sequence

1. BIOS POST
2. BIOS finds GRUB on CD-ROM (ISO 9660)
3. GRUB loads, displays brief menu (timeout=0)
4. GRUB loads `kernel.elf` via Multiboot
5. GRUB jumps to `_start` (boot.s)
6. `_start` sets up stack, calls `kernel_main()`
7. `kernel_main()` initializes VGA, serial, IDT, PIC, IDE, PCI, NIC, FS
8. Shell prompt appears on serial console

## Known Limitations

1. No memory map discovery — assumes fixed layout
2. Only boots on BIOS (no UEFI)
3. Single-threaded kernel loop (no scheduler, no processes)
4. Input via serial only (no PS/2 keyboard)
5. No graphics (VGA text mode only)
6. First file write bug
7. Network stack is polling-only (no interrupts)
8. Max 64 files, max 2 KB per file
9. PIC fully masked except syscall int (0x80)

## GDB Debugging

To debug with GDB (not yet properly configured):

```bash
# Terminal 1 — run QEMU with GDB stub
qemu-system-i386 -cdrom os.iso -hda disk.img -serial stdio \
    -no-reboot -m 128M -display none \
    -netdev user,id=n0 -device rtl8139,netdev=n0 \
    -s -S   # -s = GDB port 1234, -S = wait for GDB

# Terminal 2 — connect GDB
gdb kernel.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

Symbols are preserved (`-g3` in CFLAGS).

## Reproducible Disk Image

```bash
make clean
make
dd if=/dev/zero of=disk.img bs=1M count=2
make run
```

Builds are reproducible given the same toolchain version.
