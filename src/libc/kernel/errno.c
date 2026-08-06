/**
 * @file errno.c
 * @brief Kernel errno — single global for <errno.h>.
 *
 * In a multi-task kernel, this would be per-task. For now, single global.
 */

#include <errno.h>

int errno = 0;
