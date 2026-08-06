/**
 * @file stdlib.c
 * @brief Userspace <stdlib.h> — exit/_Exit/getenv via syscalls.
 *
 * Pure functions (atoi, strtol, qsort, bsearch, rand, abs, div, strtod)
 * live in common/stdlib.c (shared with kernel).
 * __assert_fail and abort live in kernel/stdlib.c (shared binary).
 */

#include <stdlib.h>
#include <stdint.h>

/* Provided by syscalls.c */
extern long sys_exit(int code);

/* =========================================================================
 * exit — terminate via SYS_EXIT
 * ========================================================================= */

void exit(int code) {
    sys_exit(code);
    while (1) { __asm__ volatile ("hlt"); }
}

void _Exit(int code) {
    sys_exit(code);
    while (1) { __asm__ volatile ("hlt"); }
}

/* =========================================================================
 * atexit — stub (single-task OS, no atexit chain needed)
 * ========================================================================= */

int atexit(void (*func)(void)) {
    (void)func;
    return 0;
}

/* =========================================================================
 * getenv — stub (no environment in TsOs)
 * ========================================================================= */

char *getenv(const char *name) {
    (void)name;
    return (void *)0;
}
