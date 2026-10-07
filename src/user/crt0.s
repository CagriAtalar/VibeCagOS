/* User program entry. Kernel leaves ESP -> argc, followed by argv[]. */
.section .text
.global _start
_start:
    movl  (%esp), %eax         /* argc */
    leal  4(%esp), %ebx        /* argv */
    xorl  %ebp, %ebp
    pushl %ebx
    pushl %eax
    call  user_main            /* int user_main(int argc, char **argv) */
    movl  %eax, %ebx           /* exit status */
    movl  $1, %eax             /* SYS_EXIT */
    int   $0x80
1:  jmp   1b
