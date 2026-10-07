/* clear - clear the console (SYS_CLEAR). */
#include "ulib.h"

int user_main(int argc, char **argv) {
    (void)argc; (void)argv;
    return sys_clear() < 0 ? 1 : 0;
}
