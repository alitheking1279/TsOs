/**
 * @file time.h
 * @brief Time types — freestanding libc.
 *
 * C11 §7.27. Minimal time support for kernel and userspace.
 */

#ifndef _LIBC_TIME_H
#define _LIBC_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Types (C11 §7.27) --- */
typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 100

#ifndef NULL
#define NULL ((void *)0)
#endif

/* --- Functions (C11 §7.27.2) --- */
time_t time(time_t *t);
clock_t clock(void);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_TIME_H */
