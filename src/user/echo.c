/*
 * echo - write its arguments to stdout, separated by spaces, newline last.
 * An ordinary user program; no kernel header, no direct VGA/serial access.
 */
#include "ulib.h"

int user_main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        uprintf(1, "%s%s", argv[i], i + 1 < argc ? " " : "");
    uputs(1, "\n");
    return 0;
}