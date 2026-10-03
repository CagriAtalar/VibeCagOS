/* upper — reads one line with the blocking getchar syscall, echoes it in UPPERCASE. */
#include "ulib.h"

int main(void) {
    uputs("upper> ");
    for (;;) {
        int c = sys_getchar();
        if (c == '\n' || c == '\r') break;
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        sys_putchar((char)c);
    }
    uputs("\n");
    return 0;
}
