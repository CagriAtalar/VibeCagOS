/*
 * VibeCagOS — Interrupt Stubs (x86 32-bit)
 *
 * Each ISR stub pushes a dummy error code (if the CPU doesn't push one),
 * then pushes the interrupt number, then jumps to the common handler.
 *
 * The common handler saves all general-purpose registers using PUSHA,
 * calls the C handler, then restores everything and returns.
 */

.section .text
.extern handle_interrupt

/*
 * ISR Common Path
 * Stack on entry to isr_common:
 *   [esp+0]  int_no
 *   [esp+4]  err_code
 *   [esp+8]  eip      (pushed by CPU)
 *   [esp+12] cs       (pushed by CPU)
 *   [esp+16] eflags   (pushed by CPU)
 *   [esp+20] user_esp (if ring change)
 *   [esp+24] user_ss  (if ring change)
 */
isr_common:
    /* Save all general purpose registers.
     * PUSHA pushes: eax, ecx, edx, ebx, esp, ebp, esi, edi
     * NOTE: The pushed ESP is pre-PUSHA value, used as padding in trap_frame.
     */
    pusha
    cld                         /* C code assumes DF=0 */

    /* Save the interrupted context's data segments, then switch to kernel
     * data segments. Without this the kernel would keep running with user
     * selectors (0x23) after the first ring 3 -> ring 0 transition. */
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    movw  $0x10, %ax            /* GDT_KERNEL_DATA */
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs

    /* Pass pointer to trap_frame (the current stack pointer) */
    pushl %esp
    call  handle_interrupt
    addl  $4, %esp

    /* Restore the interrupted context's segments */
    popl  %gs
    popl  %fs
    popl  %es
    popl  %ds

    /* Restore all general purpose registers */
    popa

    /* Remove int_no and err_code from stack */
    addl  $8, %esp

    /* Return from interrupt — restores eip, cs, eflags (and user_esp, user_ss if ring change) */
    iret

/* =========================================================================
 * CPU Exceptions (vectors 0–31)
 * Some push an error code automatically; for those that don't, push 0.
 * ========================================================================= */

/* 0: Divide by Zero */
.global isr0
isr0:
    pushl $0      /* dummy error code */
    pushl $0      /* interrupt number */
    jmp isr_common

/* 1: Debug */
.global isr1
isr1:
    pushl $0
    pushl $1
    jmp isr_common

/* 2: NMI */
.global isr2
isr2:
    pushl $0
    pushl $2
    jmp isr_common

/* 3: Breakpoint */
.global isr3
isr3:
    pushl $0
    pushl $3
    jmp isr_common

/* 6: Invalid Opcode */
.global isr6
isr6:
    pushl $0
    pushl $6
    jmp isr_common

/* 8: Double Fault (CPU pushes error code) */
.global isr8
isr8:
    pushl $8      /* interrupt number (error code already on stack) */
    jmp isr_common

/* 13: General Protection Fault (CPU pushes error code) */
.global isr13
isr13:
    pushl $13
    jmp isr_common

/* 14: Page Fault (CPU pushes error code) */
.global isr14
isr14:
    pushl $14
    jmp isr_common

/* =========================================================================
 * Hardware Interrupts (IRQs remapped to vectors 0x20–0x2F by PIC)
 * ========================================================================= */

/* IRQ0 → int 0x20: PIT Timer */
.global isr32
isr32:
    pushl $0      /* dummy error code */
    pushl $32     /* interrupt number */
    jmp isr_common

/* IRQ1 → int 0x21: PS/2 Keyboard */
.global isr33
isr33:
    pushl $0
    pushl $33
    jmp isr_common

/* IRQ2 → int 0x22 (cascade, usually ignored) */
.global isr34
isr34:
    pushl $0
    pushl $34
    jmp isr_common

/* IRQ11 → int 0x2B: PCI NIC (RTL8139) */
.global isr43
isr43:
    pushl $0
    pushl $43
    jmp isr_common

/* IRQ12 → int 0x2C: PS/2 Mouse */
.global isr44
isr44:
    pushl $0
    pushl $44
    jmp isr_common

/* IRQ14 → int 0x2E: IDE Primary */
.global isr46
isr46:
    pushl $0
    pushl $46
    jmp isr_common

/* =========================================================================
 * Syscall — int 0x80
 * ========================================================================= */

.global isr128
isr128:
    pushl $0      /* dummy error code */
    pushl $128    /* interrupt number */
    jmp isr_common
