/* crash — executes a privileged instruction; the kernel must kill only us. */
#include "ulib.h"

int main(void) {
    uputs("crash: executing 'cli' in ring 3...\n");
    __asm__ __volatile__("cli");
    uputs("crash: BUG, still alive after cli!\n");
    return 0;
}
