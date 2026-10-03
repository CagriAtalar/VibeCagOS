/*
 * spin — CPU-bound loop that NEVER calls yield().
 * If two copies print interleaved lines, the timer interrupt is preempting
 * ring 3 code and the scheduler is doing its job.
 */
#include "ulib.h"

int main(void) {
    int pid = sys_getpid();
    for (int round = 1; round <= 6; round++) {
        for (volatile unsigned i = 0; i < 4000000u; i++) { }
        uputs("[spin pid ");
        uputint(pid);
        uputs("] round ");
        uputint(round);
        uputs("\n");
    }
    return 0;
}
