/**
 * @file fcntl.h
 * @brief File control options — freestanding libc.
 *
 * POSIX.1-2008 §2.4. File descriptor flags and open().
 * Values match Linux x86_64 ABI for syscall compatibility.
 */

#ifndef _LIBC_FCNTL_H
#define _LIBC_FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- File status flags (POSIX.1-2008 §2.4.5) --- */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003

#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_NDELAY    0x0800
#define O_SYNC      0x101000
#define O_FSYNC     0x101000
#define O_ASYNC     0x2000

/* --- fcntl() commands --- */
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4

/* --- File descriptor flags --- */
#define FD_CLOEXEC  1

/* --- open() --- */
int open(const char *path, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_FCNTL_H */
