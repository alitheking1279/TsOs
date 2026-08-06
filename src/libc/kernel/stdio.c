/**
 * @file stdio.c
 * @brief Kernel <stdio.h> — serial-only bootstrap helpers.
 *
 * Provides stdio_set_serial() for early kernel init.
 * Defines the stdin/stdout/stderr globals (minimal FILE with fd only).
 * All FILE operations, printf, fprintf etc. live in userspace/stdio.c.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* --- Serial output (provided by kernel) --- */
extern void serial_write_char(void *dev, char c);
extern void serial_write_string(void *dev, const char *s);

/* Kernel serial device (set during init) */
static void *g_stdio_serial = (void *)0;

void stdio_set_serial(void *dev) {
    g_stdio_serial = dev;
}

/* Internal helpers — used by userspace/stdio.c for kernel-path output. */
void __kputchar(char c) {
    if (g_stdio_serial)
        serial_write_char(g_stdio_serial, c);
}

void __kputs(const char *s) {
    if (g_stdio_serial)
        serial_write_string(g_stdio_serial, s);
}
