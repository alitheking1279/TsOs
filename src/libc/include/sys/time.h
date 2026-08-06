/**
 * @file sys/time.h
 * @brief Time types — freestanding libc.
 *
 * POSIX.1-2008 §4.5.2. gettimeofday() and timeval.
 */

#ifndef _LIBC_SYS_TIME_H
#define _LIBC_SYS_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Types (POSIX.1-2008 §4.5.2) --- */
struct timeval {
    long tv_sec;    /* Seconds */
    long tv_usec;   /* Microseconds */
};

struct timezone {
    int tz_minuteswest; /* Minutes west of GMT */
    int tz_dsttime;     /* DST correction type */
};

/* --- Functions --- */
int gettimeofday(struct timeval *restrict tv, void *restrict tz);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_SYS_TIME_H */
