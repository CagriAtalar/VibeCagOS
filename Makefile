.PHONY: all clean run

QEMU := qemu-system-i386
CC := clang
AS := clang
OBJCOPY := llvm-objcopy

CFLAGS := -std=c11 -O2 -g3 -Wall -Wextra -m32 -fuse-ld=lld -fno-stack-protector \
          -ffreestanding -nostdlib -fno-pie -no-pie
ASFLAGS := -m32

# Source directories
KERNEL_DIR := src/kernel
DRIVER_DIR := src/drivers
FS_DIR := src/fs

# Source files
KERNEL_SRC := $(KERNEL_DIR)/kernel.c $(KERNEL_DIR)/common.c $(KERNEL_DIR)/user.c
DRIVER_SRC := $(DRIVER_DIR)/vga.c $(DRIVER_DIR)/ide.c
FS_SRC := $(FS_DIR)/simplefs.c

# Object files
OBJS := boot.o interrupts.o vga.o ide.o simplefs.o

all: os.iso

# Assembly
boot.o: $(KERNEL_DIR)/boot.s
	$(AS) $(ASFLAGS) -c $< -o $@

interrupts.o: $(KERNEL_DIR)/interrupts.s
	$(AS) $(ASFLAGS) -c $< -o $@

# Drivers
vga.o: $(DRIVER_DIR)/vga.c $(DRIVER_DIR)/vga.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) -I$(KERNEL_DIR) -c $< -o $@

ide.o: $(DRIVER_DIR)/ide.c $(DRIVER_DIR)/ide.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) -I$(KERNEL_DIR) -c $< -o $@

# Filesystem
simplefs.o: $(FS_DIR)/simplefs.c $(FS_DIR)/simplefs.h $(KERNEL_DIR)/common.h
	$(CC) $(CFLAGS) -I$(KERNEL_DIR) -c $< -o $@

# Kernel
kernel.elf: $(OBJS) $(KERNEL_SRC) $(KERNEL_DIR)/kernel.ld
	$(CC) $(CFLAGS) -I$(KERNEL_DIR) -I$(DRIVER_DIR) -I$(FS_DIR) \
		-Wl,-T$(KERNEL_DIR)/kernel.ld -Wl,-Map=kernel.map -o $@ \
		$(OBJS) $(KERNEL_SRC)

# Bootable ISO
os.iso: kernel.elf
	mkdir -p isodir/boot/grub
	cp kernel.elf isodir/boot/kernel.elf
	echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	echo 'set default=0' >> isodir/boot/grub/grub.cfg
	echo '' >> isodir/boot/grub/grub.cfg
	echo 'menuentry "x86 OS" {' >> isodir/boot/grub/grub.cfg
	echo '    multiboot /boot/kernel.elf' >> isodir/boot/grub/grub.cfg
	echo '    boot' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o os.iso isodir

# Create disk image
disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=2

# Run in QEMU
run: os.iso disk.img
	$(QEMU) -cdrom os.iso -hda disk.img -serial stdio -no-reboot -m 128M -display none

run-window: os.iso disk.img
	$(QEMU) -cdrom os.iso -hda disk.img -serial stdio -no-reboot -m 128M

# Clean build artifacts
clean:
	rm -f *.o *.elf *.map os.iso disk.img
	rm -rf isodir

help:
	@echo "Targets:"
	@echo "  all        - Build bootable ISO"
	@echo "  run        - Build and run in QEMU (no window)"
	@echo "  run-window - Build and run in QEMU with window"
	@echo "  clean      - Remove build artifacts"
	@echo "  help       - Show this help"
