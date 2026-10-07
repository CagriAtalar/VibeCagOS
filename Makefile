.PHONY: all clean run run-window run-gdb debug image disk help run-smp test \
        test-ring3 test-syscall test-scheduler test-usercopy test-faults \
        test-stdio test-fs test-shell test-pipe test-proc test-exec test-all \
        check-boundary kernel user tools

# ============================================================
# VibeCagOS Build System
# Version 0.6.0
# ============================================================

QEMU    := qemu-system-i386
CC      := clang
AS      := clang
OBJCOPY := $(or $(shell command -v llvm-objcopy 2>/dev/null),objcopy)
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
# make KSHELL=1  -> boot the legacy Ring-0 debug shell instead of the user shell
ifeq ($(KSHELL),1)
CFLAGS += -DKSHELL_DEBUG
endif

INCLUDES := -Isrc -Isrc/abi -I$(KERNEL_DIR) -I$(DRIVER_DIR) -I$(FS_DIR) -I$(NET_DIR)

# All object files
OBJS := boot.o interrupts.o process.o vbin.o exec.o syscall.o sysfile.o pipe.o \
        progs.o usercopy.o userblob.o \
        vga.o ide.o pci.o rtl8139.o rtc.o mouse.o gui.o \
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

all: check-boundary os.iso

# The user/kernel header boundary (§68 of the plan): user programs may include
# only src/abi/syscall.h and their own headers. Anything reaching for kernel.h,
# common.h, vmm.h, pmm.h, vfs.h or a driver header fails the build here instead
# of silently coupling userland to kernel internals.
check-boundary:
	@if grep -rEn '#include.*"(kernel\.h|common\.h|vmm\.h|pmm\.h|vfs\.h|kmalloc\.h|klog\.h|drivers/)' src/user/; then \
		echo "ERROR: src/user reaches into kernel headers (see above)"; exit 1; \
	else \
		echo "user/kernel header boundary OK"; \
	fi

# Assembly
boot.o: $(KERNEL_DIR)/boot.s
	$(AS) $(ASFLAGS) -c $< -o $@

interrupts.o: $(KERNEL_DIR)/interrupts.s
	$(AS) $(ASFLAGS) -c $< -o $@


# ---- kernel pieces split out of kernel.c ----
process.o: $(KERNEL_DIR)/process.c $(KERNEL_DIR)/kernel.h $(KERNEL_DIR)/vmm.h \
           $(KERNEL_DIR)/vbin.h src/abi/syscall.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

vbin.o: $(KERNEL_DIR)/vbin.c $(KERNEL_DIR)/vbin.h $(KERNEL_DIR)/vmm.h \
        $(KERNEL_DIR)/pmm.h $(KERNEL_DIR)/kernel.h src/abi/syscall.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

exec.o: $(KERNEL_DIR)/exec.c $(KERNEL_DIR)/kernel.h $(KERNEL_DIR)/vbin.h \
        $(KERNEL_DIR)/sysfile.h $(KERNEL_DIR)/vmm.h src/abi/syscall.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

syscall.o: $(KERNEL_DIR)/syscall.c $(KERNEL_DIR)/kernel.h $(KERNEL_DIR)/usercopy.h src/abi/syscall.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

sysfile.o: $(KERNEL_DIR)/sysfile.c $(KERNEL_DIR)/sysfile.h $(KERNEL_DIR)/pipe.h \
           $(KERNEL_DIR)/kernel.h src/abi/syscall.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

progs.o: $(KERNEL_DIR)/progs.c $(KERNEL_DIR)/kernel.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

pipe.o: $(KERNEL_DIR)/pipe.c $(KERNEL_DIR)/pipe.h $(KERNEL_DIR)/kernel.h \
       $(KERNEL_DIR)/kmalloc.h src/abi/syscall.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

usercopy.o: $(KERNEL_DIR)/usercopy.c $(KERNEL_DIR)/usercopy.h $(KERNEL_DIR)/kernel.h $(KERNEL_DIR)/vmm.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ---- user space: separate build products, flat binaries linked at USER_BASE ----
USER_CFLAGS := -std=c11 -O1 -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
               -fno-stack-protector -fuse-ld=lld -static -Wall -Wextra -Isrc/abi \
               -Wno-unused-command-line-argument

user_crt0.o: src/user/crt0.s
	$(AS) $(ASFLAGS) -c $< -o $@

# Every user program = crt0 + its own .c + the tiny user library (ulib.c).
# sh is the shell; init is the first user process. Everything in the utilities
# line is an ordinary Ring 3 program the shell spawns by name (milestone 14/15):
# `ls | head` is two /bin programs in a pipeline, not shell builtins.
USER_PROGS := sh init utest \
              ls cat echo pwd head hexdump stat \
              mkdir rmdir rm mv cp touch write \
              clear sleep kill ps uname uptime
USER_LIB_OBJS := user_crt0.o user_ulib.o

user_ulib.o: src/user/ulib.c src/user/ulib.h src/abi/syscall.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user_%.o: src/user/%.c src/user/ulib.h src/abi/syscall.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

# User programs link as ELF32 (build intermediate), then tools/vbinpack
# converts them to VBIN — the only format the kernel loads (docs/VBIN.md).
%.elf: user_%.o $(USER_LIB_OBJS) src/user/user.ld
	$(CC) $(USER_CFLAGS) -Wl,-Tsrc/user/user.ld -Wl,--build-id=none \
		-Wl,-Map=$*.map -o $@ $(USER_LIB_OBJS) user_$*.o

# Host packer: runs on the build machine, so plain cc, no -m32.
HOSTCC ?= cc
tools/vbinpack: tools/vbinpack.c
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -o $@ $<

%.vbin: %.elf tools/vbinpack
	./tools/vbinpack $< $@

# Staging only: embed the user VBIN images in the kernel image. The same
# loader path serves filesystem-backed programs.
userblob.o: $(KERNEL_DIR)/userblob.s $(addsuffix .vbin,$(USER_PROGS))
	$(AS) $(ASFLAGS) -c $< -o $@

# Never let make treat the intermediate user images as deletable: userblob.s
# `.incbin`s them and the linker needs them to exist.
.SECONDARY: $(addsuffix .elf,$(USER_PROGS)) $(addsuffix .vbin,$(USER_PROGS))

# Kernel C files
kernel.o: $(KERNEL_DIR)/kernel.c $(KERNEL_DIR)/kernel.h $(KERNEL_DIR)/common.h \
           $(KERNEL_DIR)/pmm.h $(KERNEL_DIR)/vmm.h $(KERNEL_DIR)/multiboot.h \
           $(DRIVER_DIR)/vga.h $(DRIVER_DIR)/ide.h \
           $(DRIVER_DIR)/pci.h $(DRIVER_DIR)/rtl8139.h $(DRIVER_DIR)/rtc.h \
           $(DRIVER_DIR)/mouse.h $(DRIVER_DIR)/gui.h \
           $(NET_DIR)/ethernet.h $(NET_DIR)/arp.h $(NET_DIR)/ipv4.h \
           $(NET_DIR)/icmp.h $(NET_DIR)/udp.h $(NET_DIR)/dns.h $(NET_DIR)/netconfig.h
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

# Explicit build stages: kernel / user programs / host tools / images.
kernel: kernel.elf
user: $(addsuffix .vbin,$(USER_PROGS))
tools: tools/vbinpack
image: os.iso disk.img

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

# Create disk image (16 MiB, matching VIBEFS_TOTAL_SECTORS in src/fs/vibefs.h).
# Recreated when the size does not match so an old smaller image cannot be
# written past its end by the larger filesystem.
DISK_BYTES := 16777216
disk.img:
	@if [ ! -f disk.img ] || [ "$$(stat -c%s disk.img 2>/dev/null)" != "$(DISK_BYTES)" ]; then \
		echo "Creating 16 MiB disk.img"; \
		dd if=/dev/zero of=disk.img bs=1M count=16 status=none; \
	fi

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

# ---- automated Ring-3 tests (serial output is checked by tests/run.sh) ----
test-ring3: os.iso disk.img
	@tests/run.sh ring3
test-syscall: os.iso disk.img
	@tests/run.sh syscall
test-scheduler: os.iso disk.img
	@tests/run.sh scheduler
test-usercopy: os.iso disk.img
	@tests/run.sh usercopy
test-faults: os.iso disk.img
	@tests/run.sh faults
test-stdio: os.iso disk.img
	@tests/run.sh stdio
test-fs: os.iso disk.img
	@tests/run.sh fs
test-shell: os.iso disk.img
	@tests/run.sh shell
test-pipe: os.iso disk.img
	@tests/run.sh pipe
test-proc: os.iso disk.img
	@tests/run.sh proc
test-exec: os.iso disk.img
	@tests/run.sh exec
# Two boots on one disk image: proves VibeFS data survives a reboot.
test-persist: os.iso disk.img
	@tests/persist.sh
test-all: test-ring3 test-syscall test-scheduler test-usercopy test-faults test-stdio \
          test-fs test-shell test-pipe test-proc test-exec test-persist

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
	rm -f *.o *.elf *.map *.bin os.iso disk.img
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
