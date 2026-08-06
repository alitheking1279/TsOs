/**
 * @file sys/types.h
 * @brief Primitive system data types — freestanding libc.
 *
 * POSIX.1-2008 §2.5. Common type definitions.
 */

#ifndef _LIBC_SYS_TYPES_H
#define _LIBC_SYS_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t   pid_t;
typedef int32_t   uid_t;
typedef int32_t   gid_t;
typedef int64_t   off_t;
typedef int64_t   time_t;
typedef uint32_t  mode_t;
typedef uint32_t  nlink_t;
typedef uint32_t  blksize_t;
typedef uint32_t  blkcnt_t;

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_SYS_TYPES_H */
