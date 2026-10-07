/* sleep - suspend the calling process for N milliseconds (SYS_SLEEP). */
#include "ulib.h"

int user_main(int argc, char **argv) {
    sys_sleep(argc > 1 ? (u32)atoi(argv[1]) : 1000);
    return 0;
}
