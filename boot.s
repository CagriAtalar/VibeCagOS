.section .multiboot
.align 4

multiboot_header:
    .long 0x1BADB002                                # magic
    .long 0x00000000                                # flags
    .long -(0x1BADB002 + 0x00000000)               # checksum

.section .text
.align 4
.global _start
.extern kernel_main

_start:
    # Disable interrupts
    cli
    
    # Set up stack
    mov $__stack_top, %esp
    mov %esp, %ebp
    
    # Clear EFLAGS
    pushl $0
    popf
    
    # Jump to kernel (GRUB already set up basic environment)
    call kernel_main
    
    # Halt if we return
.hang:
    cli
    hlt
    jmp .hang

