/**
 * @file stdlib.c
 * @brief Kernel-specific <stdlib.h> functions.
 *
 * __assert_fail and abort — kernel-panic-based.
 * Pure functions live in common/stdlib.c (shared with userspace).
 */

#include <stdlib.h>
#include <stdio.h>
#include <kernel/panic.h>

/* =========================================================================
 * __assert_fail — called by assert() macro on failure
 * ========================================================================= */

void __assert_fail(const char *expr, const char *file, int line) {
    /* Build a message like: "assertion 'x > 0' failed at file.c:42" */
    char msg[512];
    int pos = 0;
    const char *prefix = "assertion '";
    while (*prefix) msg[pos++] = *prefix++;
    while (*expr) msg[pos++] = *expr++;
    const char *mid = "' failed at ";
    while (*mid) msg[pos++] = *mid++;
    while (*file) msg[pos++] = *file++;
    msg[pos++] = ':';

    /* Convert line number to string */
    char lbuf[16];
    int lpos = 0;
    int lnum = line;
    if (lnum == 0) {
        lbuf[lpos++] = '0';
    } else {
        char tmp[16];
        int tpos = 0;
        while (lnum > 0) { tmp[tpos++] = '0' + (lnum % 10); lnum /= 10; }
        while (tpos > 0) lbuf[lpos++] = tmp[--tpos];
    }
    for (int i = 0; i < lpos; i++) msg[pos++] = lbuf[i];
    msg[pos++] = '\n';
    msg[pos] = '\0';

    /* Panic — the kernel halts */
    kernel_panic(msg, (void *)0);

    /* Should never return */
    for (;;) __asm__ volatile ("hlt");
}

/* =========================================================================
 * abort — raise SIGABRT-like behavior (kernel panic)
 * ========================================================================= */

void abort(void) {
    kernel_panic("abort() called", (void *)0);
    for (;;) __asm__ volatile ("hlt");
}
