/**
 * @file unistd.h
 * @brief POSIX operating system API — freestanding libc.
 *
 * POSIX.1-2008 §4. Essential system services.
 * Provides process control, file I/O, and misc POSIX functions.
 */

#ifndef _LIBC_UNISTD_H
#define _LIBC_UNISTD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Standard file descriptors --- */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* --- Types --- */
typedef int64_t  ssize_t;
typedef uint32_t useconds_t;
typedef int      pid_t;

/* --- File I/O (POSIX.1-2008 §4.3) --- */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
ssize_t lseek(int fd, ssize_t offset, int whence);
int     unlink(const char *path);
int     rmdir(const char *path);

/* --- Process control (POSIX.1-2008 §4.1) --- */
pid_t fork(void);
pid_t getpid(void);
pid_t getppid(void);
int   wait(int *status);
void  _exit(int status) __attribute__((noreturn));

/* --- Sleep (POSIX.1-2008 §4.3.1) --- */
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);

/* --- Miscellaneous (POSIX.1-2008 §4.5) --- */
int chdir(const char *path);
char *getcwd(char *buf, size_t size);

/* --- Pipe (stub) --- */
int pipe(int pipefd[2]);

/* --- Access (stub) --- */
#define F_OK 0
#define X_OK 0
#define W_OK 2
#define R_OK 4
int access(const char *path, int mode);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_UNISTD_H */
