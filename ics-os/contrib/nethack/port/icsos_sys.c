#include "hack.h"

void disable_ctrlP(void) {
    /* no-op on ICS-OS */
}

void enable_ctrlP(void) {
    /* no-op on ICS-OS */
}

void chdrive(char *dir) {
    (void)dir;
    /* no-op on ICS-OS */
}

void flushout(void) {
    (void) fflush(stdout);
}

unsigned long sys_random_seed(void) {
    time_t now = time((time_t *) 0);
    return (unsigned long) now;
}
