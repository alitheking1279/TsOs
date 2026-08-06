/**
 * @file termios.h
 * @brief Terminal I/O — freestanding libc stub.
 *
 * POSIX.1-2008 §7.2. Minimal terminal definitions for API compatibility.
 */

#ifndef _LIBC_TERMIOS_H
#define _LIBC_TERMIOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Baud rates (subset) --- */
#define B0      0
#define B9600   9600
#define B19200  19200
#define B38400  38400
#define B115200 115200

/* --- c_oflag constants --- */
#define OPOST   0x0001

/* --- c_lflag constants --- */
#define ICANON  0x0002
#define ECHO    0x0008
#define ISIG    0x0001
#define ECHONL  0x0040

/* --- c_iflag constants --- */
#define ICRNL   0x0100

/* --- tcflag_t --- */
typedef unsigned int tcflag_t;
typedef unsigned int speed_t;
typedef unsigned char cc_t;

/* --- struct termios --- */
struct termios {
    tcflag_t c_iflag;   /* Input modes */
    tcflag_t c_oflag;   /* Output modes */
    tcflag_t c_cflag;   /* Control modes */
    tcflag_t c_lflag;   /* Local modes */
    cc_t     c_cc[32];  /* Control characters */
};

/* --- Standard c_cc indices --- */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSTART  8
#define VSTOP   9

/* --- Standard actions --- */
#define TCSANOW     0
#define TCSADRAIN   1
#define TCSAFLUSH   2

/* --- Functions (stubs) --- */
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_TERMIOS_H */
