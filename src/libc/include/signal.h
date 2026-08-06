/**
 * @file signal.h
 * @brief Signal handling — freestanding libc stub.
 *
 * POSIX.1-2008 §2.4.1. Minimal signal definitions.
 * TsOs has no real signal delivery; these are stubs for API compatibility.
 */

#ifndef _LIBC_SIGNAL_H
#define _LIBC_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- Signal numbers --- */
#define SIGINT   2
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGSEGV  11
#define SIGTERM  15

#define NSIG     32

/* --- Signal handler disposition --- */
#define SIG_DFL  ((void (*)(int))0)
#define SIG_ERR  ((void (*)(int))-1)
#define SIG_IGN  ((void (*)(int))1)

/* --- Types --- */
typedef void (*sig_t)(int);

/* --- Functions (stubs) --- */
sig_t signal(int sig, sig_t handler);
int   raise(int sig);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_SIGNAL_H */
