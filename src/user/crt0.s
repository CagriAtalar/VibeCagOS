/* User program entry. The kernel delivers the start argument in EAX. */
/* The VBIN header is emitted by user.ld at the start of the image. */

.section .text
.global _start
_start:
    pushl %eax                 /* arg -> user_main(int arg) */
    call  user_main
    movl  %eax, %ebx           /* exit status */
    movl  $1, %eax             /* SYS_EXIT */
    int   $0x80
1:  jmp   1b
