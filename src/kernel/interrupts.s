.section .text
.extern handle_interrupt

# Syscall handler (int 0x80)
.global isr128
isr128:
    pushl $0        # dummy error code
    pushl $128      # interrupt number
    jmp isr_common

# Common ISR handler
isr_common:
    # Save all registers
    pushl %eax
    pushl %ecx
    pushl %edx
    pushl %ebx
    pushl %esp
    pushl %ebp
    pushl %esi
    pushl %edi
    
    # Call C handler
    pushl %esp      # Pass pointer to trap_frame
    call handle_interrupt
    addl $4, %esp   # Clean up argument
    
    # Restore registers
    popl %edi
    popl %esi
    popl %ebp
    popl %esp
    popl %ebx
    popl %edx
    popl %ecx
    popl %eax
    
    # Clean up error code and interrupt number
    addl $8, %esp
    
    iret

# Dummy ISR for other interrupts
.global isr0
isr0:
    pushl $0
    pushl $0
    jmp isr_common

