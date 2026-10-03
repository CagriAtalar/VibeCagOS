.PHONY: all clean run run-window run-gdb debug image disk help run-smp test test-user

# ============================================================
# VibeCagOS Build System
# Version 0.3.0
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
INCLUDES := -Isrc -I$(KERNEL_DIR) -I$(DRIVER_DIR) -I$(FS_DIR) -I$(NET_DIR)

# All object files
OBJS := boot.o interrupts.o userprog.o uaccess.o \
        vga.o ide.o pci.o rtl8139.o rtc.o mouse.o gui.o \
        simplefs.o \
        vibefs.o vfs.o procfs.o devfs.o \
        kmalloc.o klog.o pmm.o vmm.o \
        ethernet.o arp.o ipv4.o icmp.o udp.o dns.o \
        kernel.o common.o

# QEMU options
QEMU_COMMON := -cdrom os.iso -drive file=disk.img,format=raw,if=ide \
               -no-reboot -m 128M \
               -netdev user,id=n0 -device rtl8139,netdev=n0 \
               -serial stdio

QEMU_SMP := $(QEMU_COMMON) -smp 2

# ============================================================
# Targets
# ============================================================

all: os.iso

# ------------------------------------------------------------
# User programs (ring 3 flat binaries, embedded into the kernel)
# ------------------------------------------------------------
USER_PROGS := hello spin crash segv upper
USER_BINS  := $(addprefix user/,$(addsuffix .bin,$(USER_PROGS)))

USER_CFLAGS := -std=c11 -O2 -Wall -Wextra -m32 -fuse-ld=lld -static \
               -ffreestanding -nostdlib -fno-pie -no-pie \
               -fno-stack-protector -mno-sse -mno-mmx \
               -Isrc/kernel -Iuser -Wno-unused-command-line-argument

user/%.elf: user/%.c user/ulib.h user/user.ld $(KERNEL_DIR)/common.h
	$(CC) $(USER_CFLAGS) -Wl,-Tuser/user.ld -o $@ $<

user/%.bin: user/%.elf
	$(OBJCOPY) -O binary $< $@

.PRECIOUS: user/%.elf

userprog.o: $(KERNEL_DIR)/userprog.S $(USER_BINS)
	$(AS) $(ASFLAGS) -c $< -o $@

uaccess.o: $(KERNEL_DIR)/uaccess.c $(KERNEL_DIR)/uaccess.h $(KERNEL_DIR)/vmm.h $(KERNEL_DIR)/kernel.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Assembly
boot.o: $(KERNEL_DIR)/boot.s
	$(AS) $(ASFLAGS) -c $< -o $@

interrupts.o: $(KERNEL_DIR)/interrupts.s $(KERNEL_DIR)/kernel.h
	$(AS) $(ASFLAGS) -c $< -o $@

# Kernel C files
kernel.o: $(KERNEL_DIR)/kernel.c $(KERNEL_DIR)/kernel.h $(KERNEL_DIR)/common.h \
           $(KERNEL_DIR)/pmm.h $(KERNEL_DIR)/vmm.h $(KERNEL_DIR)/multiboot.h \
           $(FS_DIR)/simplefs.h $(DRIVER_DIR)/vga.h $(DRIVER_DIR)/ide.h \
           $(DRIVER_DIR)/pci.h $(DRIVER_DIR)/rtl8139.h $(DRIVER_DIR)/rtc.h \
           $(DRIVER_DIR)/mouse.h $(DRIVER_DIR)/gui.h \
           $(NET_DIR)/ethernet.h $(NET_DIR)/arp.h $(NET_DIR)/ipv4.h \
           $(NET_DIR)/icmp.h $(NET_DIR)/udp.h $(NET_DIR)/dns.h $(NET_DIR)/netconfig.h \
           $(KERNEL_DIR)/uaccess.h
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

rtc.o: $(DRIVER_DIR)/rtc.c $(DRIVER_DIR)/rtc.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

mouse.o: $(DRIVER_DIR)/mouse.c $(DRIVER_DIR)/mouse.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

gui.o: $(DRIVER_DIR)/gui.c $(DRIVER_DIR)/gui.h $(DRIVER_DIR)/mouse.h $(DRIVER_DIR)/rtc.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Filesystem
simplefs.o: $(FS_DIR)/simplefs.c $(FS_DIR)/simplefs.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

vfs.o: $(FS_DIR)/vfs.c $(FS_DIR)/vfs.h $(KERNEL_DIR)/kmalloc.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

vibefs.o: $(FS_DIR)/vibefs.c $(FS_DIR)/vibefs.h $(FS_DIR)/vfs.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

procfs.o: $(FS_DIR)/procfs.c $(FS_DIR)/procfs.h $(FS_DIR)/vfs.h $(KERNEL_DIR)/common.h $(KERNEL_DIR)/klog.h $(KERNEL_DIR)/pmm.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

devfs.o: $(FS_DIR)/devfs.c $(FS_DIR)/devfs.h $(FS_DIR)/vfs.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Memory
kmalloc.o: $(KERNEL_DIR)/kmalloc.c $(KERNEL_DIR)/kmalloc.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

klog.o: $(KERNEL_DIR)/klog.c $(KERNEL_DIR)/klog.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

pmm.o: $(KERNEL_DIR)/pmm.c $(KERNEL_DIR)/pmm.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

vmm.o: $(KERNEL_DIR)/vmm.c $(KERNEL_DIR)/vmm.h $(KERNEL_DIR)/pmm.h $(KERNEL_DIR)/common.h
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
        $(NET_DIR)/icmp.h $(NET_DIR)/udp.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

icmp.o: $(NET_DIR)/icmp.c $(NET_DIR)/icmp.h $(NET_DIR)/ipv4.h \
        $(NET_DIR)/ethernet.h $(NET_DIR)/byteorder.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

udp.o: $(NET_DIR)/udp.c $(NET_DIR)/udp.h $(NET_DIR)/ipv4.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

dns.o: $(NET_DIR)/dns.c $(NET_DIR)/dns.h $(NET_DIR)/udp.h $(KERNEL_DIR)/common.h
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
	$(QEMU) $(QEMU_COMMON) -display none

run-window: os.iso disk.img
	$(QEMU) $(QEMU_COMMON)

run-smp: os.iso disk.img
	$(QEMU) $(QEMU_SMP) -display none

test: os.iso disk.img
	@echo "Running QEMU integration test (serial output)..."
	@printf '\nhelp\npwd\ndate\nfree\ndevices\npci\nls\necho VibeCagOS_Automated_Test_OK > /tmp/test.txt\ncat /tmp/test.txt\ncp /tmp/test.txt /tmp/copy.txt\nhead /tmp/copy.txt\nstat /tmp/copy.txt\ncat /proc/version\ncat /proc/meminfo\ncat /etc/version\ndmesg\nexit\n' | timeout 12 $(QEMU) $(QEMU_COMMON) -display none 2>&1

# Ring 3 / scheduler / syscall-validation integration test
test-user: os.iso disk.img
	@echo "Running ring 3 integration test (serial output)..."
	@printf '\nrun hello\nrun segv\nrun crash\nusertest\nps\nexit\n' | timeout 40 $(QEMU) $(QEMU_COMMON) -display none 2>&1

# GDB debugging: Terminal 1 = make debug, Terminal 2 = gdb kernel.elf
debug: os.iso disk.img
	@echo "Starting QEMU with GDB stub on port 1234..."
	@echo "In another terminal run:"
	@echo "  gdb kernel.elf -ex 'target remote :1234' -ex 'break kernel_main' -ex 'continue'"
	$(QEMU) $(QEMU_COMMON) -display none -s -S

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
	rm -f *.o *.elf *.map os.iso disk.img user/*.elf user/*.bin
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
