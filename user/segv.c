/* segv — writes to kernel memory; the MMU must stop us, the kernel must survive. */
#include "ulib.h"

int main(void) {
    uputs("segv: writing to kernel address 0x00100000...\n");
    *(volatile int *)0x00100000 = 0xDEAD;
    uputs("segv: BUG, write to kernel memory succeeded!\n");
    return 0;
}
