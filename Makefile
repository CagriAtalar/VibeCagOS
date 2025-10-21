.PHONY: all clean run

QEMU := qemu-system-i386
CC := clang
AS := clang
OBJCOPY := llvm-objcopy

CFLAGS := -std=c11 -O2 -g3 -Wall -Wextra -m32 -fuse-ld=lld -fno-stack-protector -ffreestanding -nostdlib -fno-pie -no-pie
ASFLAGS := -m32

all: kernel.elf

# User program (shell)
shell.elf: shell.c user.c common.c user.ld
	$(CC) $(CFLAGS) -Wl,-Tuser.ld -Wl,-Map=shell.map -o $@ shell.c user.c common.c

shell.bin: shell.elf
	$(OBJCOPY) --set-section-flags .bss=alloc,contents -O binary $< $@

shell.bin.o: shell.bin
	$(OBJCOPY) -Ibinary -Oelf32-i386 $< $@

# Embedded files
disk/hello.txt.o: disk/hello.txt
	$(OBJCOPY) -Ibinary -Oelf32-i386 $< $@

disk/meow.txt.o: disk/meow.txt
	$(OBJCOPY) -Ibinary -Oelf32-i386 $< $@

# Kernel
boot.o: boot.s
	$(AS) $(ASFLAGS) -c $< -o $@

interrupts.o: interrupts.s
	$(AS) $(ASFLAGS) -c $< -o $@

embedded_fs.o: embedded_fs.c embedded_fs.h common.h
	$(CC) $(CFLAGS) -c embedded_fs.c -o $@

vga.o: vga.c vga.h common.h
	$(CC) $(CFLAGS) -c vga.c -o $@

keyboard.o: keyboard.c keyboard.h common.h
	$(CC) $(CFLAGS) -c keyboard.c -o $@

kernel.elf: boot.o interrupts.o embedded_fs.o vga.o keyboard.o kernel.c common.c disk/hello.txt.o disk/meow.txt.o kernel.ld
	$(CC) $(CFLAGS) -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o $@ \
		boot.o interrupts.o embedded_fs.o vga.o keyboard.o kernel.c common.c disk/hello.txt.o disk/meow.txt.o

# Disk image
disk.tar: disk/*.txt
	cd disk && tar cf ../disk.tar --format=ustar *.txt

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

run: os.iso
	$(QEMU) -cdrom os.iso -serial stdio -no-reboot -m 128M -display none

run-window: os.iso
	$(QEMU) -cdrom os.iso -serial stdio -no-reboot -m 128M

clean:
	rm -f *.o *.elf *.bin *.map disk.tar shell.bin.o os.iso disk/*.o
	rm -rf isodir

help:
	@echo "Targets:"
	@echo "  all      - Build kernel"
	@echo "  run      - Build and run in QEMU"
	@echo "  clean    - Remove build artifacts"
	@echo "  help     - Show this help"

