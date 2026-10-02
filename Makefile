.PHONY: all clean run run-window run-gdb debug image disk help

# ============================================================
# VibeCagOS Build System
# Version 0.2.0
# ============================================================

QEMU    := qemu-system-i386
CC      := clang
AS      := clang
OBJCOPY := llvm-objcopy
OBJDUMP := llvm-objdump

# Compiler flags
CFLAGS  := -std=c11 -O2 -g3 -Wall -Wextra \
            -m32 -fuse-ld=lld \
            -fno-stack-protector \
            -ffreestanding -nostdlib \
            -fno-pie -no-pie \
            -Wno-unused-command-line-argument

ASFLAGS := -m32

# Source directories
KERNEL_DIR := src/kernel
DRIVER_DIR := src/drivers
FS_DIR     := src/fs
NET_DIR    := src/net

# Common include path
INCLUDES := -I$(KERNEL_DIR) -I$(DRIVER_DIR) -I$(FS_DIR) -I$(NET_DIR)

# All object files
OBJS := boot.o interrupts.o \
        vga.o ide.o pci.o rtl8139.o \
        simplefs.o \
        ethernet.o arp.o ipv4.o icmp.o \
        kernel.o common.o

# QEMU options
QEMU_COMMON := -cdrom os.iso -hda disk.img \
               -no-reboot -m 128M \
               -netdev user,id=n0 -device rtl8139,netdev=n0

# ============================================================
# Targets
# ============================================================

all: os.iso

# Assembly
boot.o: $(KERNEL_DIR)/boot.s
	$(AS) $(ASFLAGS) -c $< -o $@

interrupts.o: $(KERNEL_DIR)/interrupts.s
	$(AS) $(ASFLAGS) -c $< -o $@

# Kernel C files
kernel.o: $(KERNEL_DIR)/kernel.c $(KERNEL_DIR)/kernel.h $(KERNEL_DIR)/common.h \
           $(FS_DIR)/simplefs.h $(DRIVER_DIR)/vga.h $(DRIVER_DIR)/ide.h \
           $(DRIVER_DIR)/pci.h $(DRIVER_DIR)/rtl8139.h \
           $(NET_DIR)/ethernet.h $(NET_DIR)/arp.h $(NET_DIR)/ipv4.h \
           $(NET_DIR)/icmp.h $(NET_DIR)/netconfig.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

common.o: $(KERNEL_DIR)/common.c $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Drivers
vga.o: $(DRIVER_DIR)/vga.c $(DRIVER_DIR)/vga.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

ide.o: $(DRIVER_DIR)/ide.c $(DRIVER_DIR)/ide.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

pci.o: $(DRIVER_DIR)/pci.c $(DRIVER_DIR)/pci.h $(KERNEL_DIR)/common.h $(KERNEL_DIR)/kernel.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

rtl8139.o: $(DRIVER_DIR)/rtl8139.c $(DRIVER_DIR)/rtl8139.h $(DRIVER_DIR)/pci.h \
            $(KERNEL_DIR)/common.h $(KERNEL_DIR)/kernel.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Filesystem
simplefs.o: $(FS_DIR)/simplefs.c $(FS_DIR)/simplefs.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Network stack
ethernet.o: $(NET_DIR)/ethernet.c $(NET_DIR)/ethernet.h $(NET_DIR)/arp.h \
             $(NET_DIR)/ipv4.h $(NET_DIR)/byteorder.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

arp.o: $(NET_DIR)/arp.c $(NET_DIR)/arp.h $(NET_DIR)/ethernet.h \
       $(NET_DIR)/byteorder.h $(NET_DIR)/netconfig.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

ipv4.o: $(NET_DIR)/ipv4.c $(NET_DIR)/ipv4.h $(NET_DIR)/ethernet.h \
        $(NET_DIR)/arp.h $(NET_DIR)/byteorder.h $(NET_DIR)/netconfig.h \
        $(NET_DIR)/icmp.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

icmp.o: $(NET_DIR)/icmp.c $(NET_DIR)/icmp.h $(NET_DIR)/ipv4.h \
        $(NET_DIR)/ethernet.h $(NET_DIR)/byteorder.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Link kernel ELF
kernel.elf: $(OBJS) $(KERNEL_DIR)/kernel.ld
	$(CC) $(CFLAGS) $(INCLUDES) \
		-Wl,-T$(KERNEL_DIR)/kernel.ld \
		-Wl,-Map=kernel.map \
		-o $@ $(OBJS)

# Create bootable ISO
os.iso: kernel.elf
	mkdir -p isodir/boot/grub
	cp kernel.elf isodir/boot/kernel.elf
	@printf 'set timeout=0\nset default=0\n\nmenuentry "VibeCagOS" {\n    multiboot /boot/kernel.elf\n    boot\n}\n' > isodir/boot/grub/grub.cfg
	grub-mkrescue -o os.iso isodir

# Create disk image
disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=2 status=none

# ============================================================
# Run targets
# ============================================================

run: os.iso disk.img
	$(QEMU) $(QEMU_COMMON) -serial stdio -display none

run-window: os.iso disk.img
	$(QEMU) $(QEMU_COMMON) -serial stdio

# GDB debugging: Terminal 1 = make debug, Terminal 2 = gdb kernel.elf
debug: os.iso disk.img
	@echo "Starting QEMU with GDB stub on port 1234..."
	@echo "In another terminal run:"
	@echo "  gdb kernel.elf -ex 'target remote :1234' -ex 'break kernel_main' -ex 'continue'"
	$(QEMU) $(QEMU_COMMON) -serial stdio -display none -s -S

# Disassemble kernel
disassemble: kernel.elf
	$(OBJDUMP) -d kernel.elf | less

# Show kernel symbols
symbols: kernel.elf
	nm kernel.elf | sort

# ============================================================
# Utility
# ============================================================

clean:
	rm -f *.o *.elf *.map os.iso disk.img
	rm -rf isodir

help:
	@echo "VibeCagOS Build Targets:"
	@echo ""
	@echo "  make             Build bootable ISO"
	@echo "  make run         Build and run (serial console only)"
	@echo "  make run-window  Build and run with QEMU window (VGA visible)"
	@echo "  make debug       Run with GDB stub (port 1234, CPU halted)"
	@echo "  make disassemble Disassemble kernel.elf"
	@echo "  make symbols     List kernel symbols"
	@echo "  make clean       Remove build artifacts"
	@echo ""
	@echo "Requirements (WSL2 Ubuntu):"
	@echo "  clang lld qemu-system-x86 grub-pc-bin grub-common xorriso mtools"
