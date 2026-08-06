/**
 * @file errno.c
 * @brief Userspace errno global variable.
 *
 * Each thread/task has its own errno. In our single-task userspace,
 * this is a simple global. The kernel has its own in kernel/errno.c.
 */

#include <errno.h>

int errno = 0;
