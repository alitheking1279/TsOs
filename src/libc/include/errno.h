/**
 * @file errno.h
 * @brief Error numbers — freestanding libc.
 *
 * C11 §7.5. Minimal set of error codes for kernel and userspace.
 */

#ifndef _LIBC_ERRNO_H
#define _LIBC_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

extern int errno;

#define EDOM        33   /* Math argument out of domain */
#define ERANGE      34   /* Result too large */
#define EILSEQ      84   /* Invalid multibyte sequence */
#define ENOMEM      12   /* Out of memory */
#define EINVAL      22   /* Invalid argument */
#define ENOENT       2   /* No such file or directory */
#define EIO          5   /* I/O error */
#define EBADF        9   /* Bad file descriptor */
#define EACCES      13   /* Permission denied */
#define EBUSY       16   /* Device or resource busy */
#define EEXIST      17   /* File exists */
#define ENOTDIR     20   /* Not a directory */
#define EISDIR      21   /* Is a directory */
#define EMFILE      24   /* Too many open files */
#define ENOSPC      28   /* No space left on device */
#define EPIPE       32   /* Broken pipe */
#define EAGAIN      11   /* Try again (non-blocking) */
#define EWOULDBLOCK 11   /* Same as EAGAIN */
#define EINTR        4   /* Interrupted system call */

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_ERRNO_H */
