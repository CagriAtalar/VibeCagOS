#!/bin/bash
set -xue

QEMU=qemu-system-i386
CC=/usr/bin/clang
AS=/usr/bin/clang
OBJCOPY=/usr/bin/llvm-objcopy

CFLAGS="-std=c11 -O2 -g3 -Wall -Wextra -m32 -fuse-ld=lld -fno-stack-protector -ffreestanding -nostdlib -fno-pie -no-pie"
ASFLAGS="-m32"

# Build the shell (user program)
$CC $CFLAGS -Wl,-Tuser.ld -Wl,-Map=shell.map -o shell.elf shell.c user.c common.c
$OBJCOPY --set-section-flags .bss=alloc,contents -O binary shell.elf shell.bin
$OBJCOPY -Ibinary -Oelf32-i386 shell.bin shell.bin.o

# Build boot assembly
$AS $ASFLAGS -c boot.s -o boot.o
$AS $ASFLAGS -c interrupts.s -o interrupts.o

# Embed disk files
$OBJCOPY -Ibinary -Oelf32-i386 disk/hello.txt disk/hello.txt.o
$OBJCOPY -Ibinary -Oelf32-i386 disk/meow.txt disk/meow.txt.o

# Build embedded filesystem
$CC $CFLAGS -c embedded_fs.c -o embedded_fs.o

# Build VGA driver
$CC $CFLAGS -c vga.c -o vga.o

# Build keyboard driver
$CC $CFLAGS -c keyboard.c -o keyboard.o

# Build the kernel
$CC $CFLAGS -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o kernel.elf \
    boot.o interrupts.o embedded_fs.o vga.o keyboard.o kernel.c common.c disk/hello.txt.o disk/meow.txt.o

# Create disk image
(cd disk && tar cf ../disk.tar --format=ustar *.txt)

# Create a bootable image using multiboot with GRUB
# Create grub config
mkdir -p isodir/boot/grub
cp kernel.elf isodir/boot/kernel.elf

cat > isodir/boot/grub/grub.cfg << 'EOF'
set timeout=0
set default=0

menuentry "x86 OS" {
    multiboot /boot/kernel.elf
    boot
}
EOF

# Create ISO
grub-mkrescue -o os.iso isodir 2>/dev/null || {
    echo "grub-mkrescue not available, trying direct boot..."
    # Fallback: try to boot ELF directly with -kernel
    $QEMU -kernel kernel.elf -serial stdio -no-reboot \
        -drive id=drive0,file=disk.tar,format=raw,if=none \
        -device virtio-blk-pci,drive=drive0 \
        -m 128M
    exit 0
}

# Run QEMU with ISO (no disk needed - files embedded in kernel!)
# -display none: Hide the graphical window (we only use serial console)
$QEMU -cdrom os.iso -serial stdio -no-reboot -m 128M -display none

