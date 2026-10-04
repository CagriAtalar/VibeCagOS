/*
 * VibeCagOS - Interrupt entry/exit (x86 32-bit)
 *
 * Every vector gets a tiny stub that normalises the stack so that all
 * traps share one layout, then jumps to isr_common.
 *
 * Stack layout seen by handle_interrupt(struct trap_frame *f), low -> high:
 *
 *   [esp+0x00] gs  \
 *   [esp+0x04] fs   | saved user (or kernel) data segments
 *   [esp+0x08] es   |
 *   [esp+0x0C] ds  /
 *   [esp+0x10] edi \
 *   [esp+0x14] esi  |
 *   [esp+0x18] ebp  | pusha block
 *   [esp+0x1C] esp  | (value before pusha, unusable)
 *   [esp+0x20] ebx  |
 *   [esp+0x24] edx  |
 *   [esp+0x28] ecx  |
 *   [esp+0x2C] eax /
 *   [esp+0x30] int_no    pushed by stub
 *   [esp+0x34] err_code  pushed by CPU, or 0 pushed by stub
 *   [esp+0x38] eip   \
 *   [esp+0x3C] cs     | pushed by CPU
 *   [esp+0x40] eflags/
 *   [esp+0x44] user_esp \ only when CPL changed (CS & 3 == 3)
 *   [esp+0x48] user_ss  /
 *
 * Because the whole frame lives on the PER-PROCESS KERNEL STACK
 * (TSS.esp0), switching processes inside handle_interrupt() is safe:
 * the old process's frame stays intact on its own stack and is resumed
 * later by returning up through isr_common -> trapret -> iret.
 *
 * `trapret` is also the first "return address" of a brand-new user
 * process: process creation builds a fake trap frame on its kernel
 * stack and switch_context() "returns" into trapret.
 */

.section .text
.extern handle_interrupt

isr_common:
    /* Build the frame documented above (gs ends up lowest). */
    pusha                       /* edi lowest ... eax highest */
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    movw  $0x10, %ax            /* kernel data selector */
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs
    cld
    pushl %esp                  /* arg: struct trap_frame * */
    call  handle_interrupt
    addl  $4, %esp

.global trapret
trapret:
    popl  %gs
    popl  %fs
    popl  %es
    popl  %ds
    popa
    addl  $8, %esp              /* drop int_no + err_code */
    iret

/*
 * switch_context(uint32_t *old_sp, uint32_t new_sp)   [cdecl]
 *
 * Saves callee-saved registers on the current kernel stack, stores ESP in
 * *old_sp, loads new_sp, restores the other stack's callee-saved registers
 * and returns on THAT stack.
 *
 *   [esp+4] old_sp (pointer)   [esp+8] new_sp (value)
 *
 * Stack of a switched-out task (low -> high): edi esi ebx ebp ret_addr
 * Cooperative-only primitive: it is called from schedule() which itself
 * runs either inside an ISR/syscall or with interrupts disabled.
 */
.global switch_context
switch_context:
    movl  4(%esp), %eax
    movl  8(%esp), %edx
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl  %esp, (%eax)
    movl  %edx, %esp
    popl  %edi
    popl  %esi
    popl  %ebx
    popl  %ebp
    ret

/* ---- stub generators ------------------------------------------------- */
.macro ISR_NOERR n
.global isr\n
isr\n:
    pushl $0
    pushl $\n
    jmp   isr_common
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    pushl $\n                   /* CPU already pushed the error code */
    jmp   isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

/* IRQ0..15 -> vectors 32..47 */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/* int 0x80 - user syscall gate (DPL 3) */
ISR_NOERR 128

/* Table of vectors 0..47 so idt_init() can loop. */
.section .rodata
.global isr_stub_table
isr_stub_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .long isr\n
.endr
