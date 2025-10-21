# x86 Operating System - Educational Project

A minimal x86 (i386) operating system with embedded file system, built for learning OS fundamentals.

## Features

- ✅ 32-bit protected mode
- ✅ GRUB bootloader (Multiboot)
- ✅ VGA text mode + Serial console
- ✅ PS/2 Keyboard driver (works in VirtualBox!)
- ✅ Interrupt handling (PIC + IRQs)
- ✅ Embedded file system (no disk driver needed!)
- ✅ Full file management (create, read, write, delete)
- ✅ Interactive shell
- ✅ Boots in QEMU and VirtualBox

## Quick Start

### Build and Run

```bash
cd iksseksen

# Build
make clean && make os.iso

# Run in QEMU (no window)
make run

# Run in QEMU (with window to see VGA)
make run-window

# Or use shell script
./run.sh
```

### VirtualBox

1. Create VM: Other/Unknown (32-bit), 128MB RAM, No hard disk
2. Settings → Storage → Attach `os.iso` to CD drive
3. Start VM
4. Output appears in VM window (VGA text mode)

## Available Commands

```
help            - Show all commands
ls              - List all files
cat <file>      - Display file contents
readf <file>    - Read and display file
addf <file>     - Create new file
writef <file>   - Write content to file (multi-line)
rm <file>       - Delete file
hello           - Print greeting
exit            - Exit shell
```

## Usage Example

```bash
> ls
Files:
  hello.txt (19 bytes) [embedded]
  meow.txt (15 bytes) [embedded]

> cat hello.txt
Hello from x86 OS!

> addf notes.txt
File 'notes.txt' created successfully

> writef notes.txt
Enter content (end with empty line):
| My first note
| Second line
| 
Wrote 26 bytes to 'notes.txt'

> ls
Files:
  hello.txt (19 bytes) [embedded]
  meow.txt (15 bytes) [embedded]
  notes.txt (26 bytes) [created]

> readf notes.txt
My first note
Second line

> rm notes.txt
File 'notes.txt' deleted

> exit
Goodbye!
```

## Architecture

- **Boot**: GRUB → Multiboot → kernel
- **Display**: VGA text mode (0xB8000) + Serial (COM1)
- **Input**: PS/2 Keyboard (port 0x60) + Serial
- **Interrupts**: PIC (8259) with IRQ1 for keyboard
- **Files**: Embedded in kernel binary + RAM storage
- **File System**: Max 10 files, 2KB each
- **Simple**: No processes, no VirtIO, no complexity!

## File System

### Embedded Files
- Compiled into kernel at build time
- `hello.txt` and `meow.txt` included
- Add more by editing `disk/` directory

### Runtime Files
- Created with `addf` command
- Stored in RAM (2KB max per file)
- Lost on reboot (educational purposes)
- Up to 10 total files

## Technical Details

- **Language**: C11 + x86 Assembly
- **Bootloader**: GRUB (Multiboot)
- **Memory**: Identity-mapped, no paging
- **I/O**: VGA text mode + Serial port
- **No drivers**: No disk, no VirtIO
- **~700 lines**: Core OS code

## Building

### Requirements
- Clang with 32-bit support
- LLD linker
- QEMU (qemu-system-i386)
- GRUB tools (grub-mkrescue)
- llvm-objcopy

### Ubuntu/Debian
```bash
sudo apt install clang lld qemu-system-x86 llvm \
    grub-pc-bin grub-common xorriso mtools
```

### Build Process
```bash
make clean    # Clean old builds
make all      # Build kernel
make os.iso   # Create bootable ISO
make run      # Build and run
```

## Files

- `kernel.c` - Main kernel
- `kernel.h` - Kernel headers
- `embedded_fs.c/h` - File system
- `vga.c/h` - VGA text mode driver
- `keyboard.c/h` - PS/2 keyboard driver
- `common.c/h` - Utility functions
- `boot.s` - Boot assembly
- `interrupts.s` - Interrupt handlers
- `kernel.ld` - Linker script
- `Makefile` - Build system
- `disk/` - Files to embed

## Credits

Based on [Operating System in 1,000 Lines](https://github.com/nuta/operating-system-in-1000-lines) (RISC-V)

This x86 port by porting RISC-V to i386 architecture with simplified embedded file system.

## License

MIT License (same as original project)

## Educational Value

Perfect for learning:
- x86 boot process
- VGA text mode
- Basic file systems
- OS fundamentals without complexity
- Binary embedding techniques

**No disk drivers, no VirtIO, no processes - just pure OS basics!** 🚀

---

**Directory**: iksseksen (Turkish: "eighty-six" = x86)  
**Status**: Complete and working  
**ISO**: Ready for QEMU and VirtualBox
