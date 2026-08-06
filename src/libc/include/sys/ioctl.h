/**
 * @file sys/ioctl.h
 * @brief Device I/O control — freestanding libc stub.
 *
 * POSIX.1-2008 §4.5. Minimal ioctl definitions.
 */

#ifndef _LIBC_SYS_IOCTL_H
#define _LIBC_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_SYS_IOCTL_H */
