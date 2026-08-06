/**
 * @file posix.c
 * @brief POSIX function wrappers — bridges standard C API to syscall layer.
 *
 * All objects link into one kernel image. This file provides the standard
 * POSIX names (open, close, read, write, etc.) that map to sys_* calls.
 * Stubs are provided for functions that don't yet have kernel support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include "syscalls.h"

/* =========================================================================
 * File I/O — bridge to sys_* calls
 * ========================================================================= */

int open(const char *path, int flags, ...) {
    return (int)sys_open(path, flags);
}

int close(int fd) {
    return (int)sys_close(fd);
}

ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)sys_read(fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)sys_write(fd, buf, count);
}

ssize_t lseek(int fd, ssize_t offset, int whence) {
    return (ssize_t)sys_lseek(fd, (long)offset, whence);
}

int fstat(int fd, struct stat *buf) {
    return (int)sys_fstat(fd, buf);
}

int stat(const char *restrict path, struct stat *buf) {
    int fd = (int)sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    int ret = fstat(fd, buf);
    sys_close(fd);
    return ret;
}

int unlink(const char *path) {
    return (int)sys_unlink(path);
}

int rmdir(const char *path) {
    return (int)sys_rmdir(path);
}

int mkdir(const char *path, uint32_t mode) {
    return (int)sys_mkdir(path, mode);
}

int rename(const char *oldpath, const char *newpath) {
    return (int)sys_rename(oldpath, newpath);
}

/* =========================================================================
 * Process control
 * ========================================================================= */

pid_t fork(void) {
    return (pid_t)sys_fork();
}

pid_t getpid(void) {
    return (pid_t)sys_getpid();
}

pid_t getppid(void) {
    return (pid_t)sys_getppid();
}

int wait(int *status) {
    return (int)sys_wait(status);
}

/* =========================================================================
 * Sleep — implemented via SYS_USLEEP
 * ========================================================================= */

int usleep(useconds_t usec) {
    sys_usleep(usec);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    while (seconds > 0) {
        sys_usleep(1000000);
        seconds--;
    }
    return 0;
}

/* =========================================================================
 * Time — via gettimeofday syscall
 * ========================================================================= */

int gettimeofday(struct timeval *restrict tv, void *restrict tz) {
    (void)tz;
    if (tv) {
        long sec = 0;
        sys_gettimeofday(&sec);
        tv->tv_sec = (time_t)sec;
        tv->tv_usec = 0;
    }
    return 0;
}

clock_t clock(void) {
    return 0;
}

time_t time(time_t *t) {
    long sec = 0;
    sys_gettimeofday(&sec);
    if (t) *t = (time_t)sec;
    return (time_t)sec;
}

/* =========================================================================
 * Signal — stubs (no signal delivery in TsOs)
 * ========================================================================= */

sig_t signal(int sig, sig_t handler) {
    (void)sig;
    (void)handler;
    return SIG_ERR;
}

int raise(int sig) {
    (void)sig;
    return -1;
}

/* =========================================================================
 * Miscellaneous POSIX stubs
 * ========================================================================= */

int chdir(const char *path) {
    (void)path;
    return -1;
}

char *getcwd(char *buf, size_t size) {
    (void)buf;
    (void)size;
    return (void *)0;
}

int pipe(int pipefd[2]) {
    (void)pipefd;
    return -1;
}

int access(const char *path, int mode) {
    (void)path;
    (void)mode;
    return -1;
}

/* =========================================================================
 * Terminal I/O — stubs
 * ========================================================================= */

int tcgetattr(int fd, struct termios *t) {
    (void)fd;
    (void)t;
    return 0;
}

int tcsetattr(int fd, int actions, const struct termios *t) {
    (void)fd;
    (void)actions;
    (void)t;
    return 0;
}

speed_t cfgetispeed(const struct termios *t) {
    (void)t;
    return B9600;
}

speed_t cfgetospeed(const struct termios *t) {
    (void)t;
    return B9600;
}

int ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    (void)request;
    return -1;
}

/* =========================================================================
 * inttypes.h helper
 * ========================================================================= */

imaxdiv_t imaxdiv(long numer, long denom) {
    imaxdiv_t result;
    result.quot = numer / denom;
    result.rem  = numer % denom;
    return result;
}
