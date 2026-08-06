/**
 * @file stdio.c
 * @brief Userspace <stdio.h> — full FILE with buffered I/O via syscalls.
 *
 * This is THE single implementation of all FILE operations for the
 * linked binary.  Functions that only format to a buffer (vsnprintf,
 * snprintf, sprintf, sscanf) live in common/vsnprintf.c.
 *
 * CRITICAL: fprintf/printf here use sys_write, which requires ring 3.
 * Kernel code must NOT call these — use print.c helpers instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include "syscalls.h"

/* =========================================================================
 * Default buffer size
 * ========================================================================= */

#define FBUF_SIZE 4096

/* =========================================================================
 * stdin / stdout / stderr — the three standard streams
 * ========================================================================= */

static FILE g_stdin_data  = { .fd = 0, .buf_mode = _IONBF };
static FILE g_stdout_data = { .fd = 1, .buf_mode = _IONBF };
static FILE g_stderr_data = { .fd = 2, .buf_mode = _IONBF };

FILE *stdin  = &g_stdin_data;
FILE *stdout = &g_stdout_data;
FILE *stderr = &g_stderr_data;

/* =========================================================================
 * Buffer management helpers
 * ========================================================================= */

static void buf_init(FILE *f) {
    if (f->buf) return;
    f->buf = (unsigned char *)malloc(FBUF_SIZE);
    if (f->buf) {
        f->buf_size = FBUF_SIZE;
        f->buf_pos  = 0;
        f->buf_len  = 0;
    }
}

static void buf_free(FILE *f) {
    if (f->buf) {
        free(f->buf);
        f->buf = (void *)0;
        f->buf_size = 0;
    }
}

/* Flush write buffer to fd. Returns 0 on success, -1 on error. */
static int buf_flush(FILE *f) {
    if (!f || !f->buf || f->buf_pos == 0) return 0;

    size_t total = f->buf_pos;
    size_t written = 0;
    while (written < total) {
        long n = sys_write(f->fd, f->buf + written, total - written);
        if (n < 0) { f->error_flag = 1; return -1; }
        if (n == 0) { f->error_flag = 1; return -1; }
        written += (size_t)n;
    }
    f->buf_pos = 0;
    return 0;
}

/* Refill read buffer.  Returns first byte or EOF. */
static int buf_refill(FILE *f) {
    if (!f || !f->buf) return EOF;
    long n = sys_read(f->fd, f->buf, f->buf_size);
    if (n <= 0) { f->eof_flag = (n == 0); f->error_flag = (n < 0); return EOF; }
    f->buf_len = (size_t)n;
    f->buf_pos = 0;
    return f->buf[f->buf_pos++];
}

/* =========================================================================
 * fopen / fclose
 * ========================================================================= */

FILE *fopen(const char *restrict path, const char *restrict mode) {
    int flags = 0;
    int is_writing = 0;

    /* Parse mode string (POSIX.1-2008 §7.21.5.3). */
    switch (mode[0]) {
    case 'r':
        flags = O_RDONLY;
        break;
    case 'w':
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        is_writing = 1;
        break;
    case 'a':
        flags = O_WRONLY | O_CREAT | O_APPEND;
        is_writing = 1;
        break;
    default:
        return (void *)0;
    }
    if (mode[1] == '+') {
        flags = (flags & ~O_ACCMODE) | O_RDWR;
        is_writing = 1;
    }

    int fd = (int)sys_open(path, flags);
    if (fd < 0) return (void *)0;

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { sys_close(fd); return (void *)0; }

    memset(f, 0, sizeof(FILE));
    f->fd         = fd;
    f->mode_flags = flags;
    f->buf_mode   = is_writing ? _IOFBF : _IOFBF;
    f->eof_flag   = 0;
    f->error_flag = 0;

    buf_init(f);
    return f;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;

    int ret = 0;
    if (stream->mode_flags & (O_WRONLY | O_RDWR | O_APPEND)) {
        if (buf_flush(stream) != 0) ret = EOF;
    }
    if (sys_close(stream->fd) != 0) ret = EOF;
    buf_free(stream);
    free(stream);
    return ret;
}

/* =========================================================================
 * fread / fwrite — buffered I/O
 * ========================================================================= */

size_t fread(void *restrict ptr, size_t size, size_t nmemb,
             FILE *restrict stream) {
    if (!stream || size == 0 || nmemb == 0) return 0;

    size_t total = size * nmemb;
    size_t done = 0;
    unsigned char *dst = (unsigned char *)ptr;

    while (done < total) {
        /* Refill buffer if empty. */
        if (stream->buf_pos >= stream->buf_len) {
            if (stream->eof_flag) break;
            int c = buf_refill(stream);
            if (c == EOF) break;
        }
        dst[done++] = stream->buf[stream->buf_pos++];
    }
    return done / size;
}

size_t fwrite(const void *restrict ptr, size_t size, size_t nmemb,
              FILE *restrict stream) {
    if (!stream || size == 0 || nmemb == 0) return 0;

    size_t total = size * nmemb;
    const unsigned char *src = (const unsigned char *)ptr;
    size_t done = 0;

    /* Unbuffered (or line-buffered with flush on newline): write directly. */
    if (stream->buf_mode == _IONBF || !stream->buf) {
        while (done < total) {
            long n = sys_write(stream->fd, src + done, total - done);
            if (n < 0) { stream->error_flag = 1; break; }
            done += (size_t)n;
        }
        return done / size;
    }

    /* Buffered: copy into write buffer, flush when full. */
    buf_init(stream);
    while (done < total) {
        if (stream->buf_pos >= stream->buf_size) {
            if (buf_flush(stream) != 0) break;
        }
        stream->buf[stream->buf_pos++] = src[done++];
    }
    return done / size;
}

/* =========================================================================
 * fseek / ftell / rewind
 * ========================================================================= */

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;

    /* Flush write buffer before seeking. */
    if (stream->mode_flags & (O_WRONLY | O_RDWR | O_APPEND))
        buf_flush(stream);

    long ret = sys_lseek(stream->fd, (long)offset, whence);
    if (ret < 0) return -1;

    /* Invalidate read buffer. */
    stream->buf_pos = 0;
    stream->buf_len = 0;
    stream->eof_flag = 0;
    return 0;
}

long ftell(FILE *stream) {
    if (!stream) return -1;
    long ret = sys_lseek(stream->fd, 0, SEEK_CUR);
    if (ret < 0) return -1;
    /* Adjust for buffered read-ahead. */
    if (stream->buf_len > 0 && stream->buf_pos <= stream->buf_len)
        ret -= (long)(stream->buf_len - stream->buf_pos);
    return ret;
}

void rewind(FILE *stream) {
    if (stream) fseek(stream, 0, SEEK_SET);
}

/* =========================================================================
 * fflush
 * ========================================================================= */

int fflush(FILE *stream) {
    if (!stream) return 0;
    return buf_flush(stream);
}

/* =========================================================================
 * setvbuf
 * ========================================================================= */

int setvbuf(FILE *restrict stream, char *restrict buf, int mode, size_t size) {
    if (!stream) return -1;
    buf_flush(stream);
    buf_free(stream);

    stream->buf_mode = mode;
    if (mode == _IONBF) {
        stream->buf = (void *)0;
        stream->buf_size = 0;
    } else if (buf && size > 0) {
        stream->buf = (unsigned char *)buf;
        stream->buf_size = size;
    } else {
        buf_init(stream);
    }
    stream->buf_pos = 0;
    stream->buf_len = 0;
    return 0;
}

/* =========================================================================
 * Unformatted output
 * ========================================================================= */

int fputc(int c, FILE *stream) {
    if (!stream) return EOF;

    unsigned char ch = (unsigned char)c;

    if (stream->buf_mode == _IONBF || !stream->buf) {
        long n = sys_write(stream->fd, &ch, 1);
        return (n == 1) ? c : EOF;
    }

    buf_init(stream);
    if (stream->buf_pos >= stream->buf_size) {
        if (buf_flush(stream) != 0) return EOF;
    }
    stream->buf[stream->buf_pos++] = ch;

    if (stream->buf_mode == _IOLBF && ch == '\n')
        buf_flush(stream);

    return c;
}

int fputs(const char *restrict s, FILE *stream) {
    if (!stream || !s) return EOF;
    while (*s) {
        if (fputc(*s++, stream) == EOF) return EOF;
    }
    return 0;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return 0;
}

int putchar(int c) {
    return fputc(c, stdout);
}

/* =========================================================================
 * Unformatted input
 * ========================================================================= */

int fgetc(FILE *stream) {
    if (!stream) return EOF;
    if (stream->buf_mode != _IONBF && stream->buf) {
        if (stream->buf_pos >= stream->buf_len) {
            if (stream->eof_flag) return EOF;
            int c = buf_refill(stream);
            return c;
        }
        return stream->buf[stream->buf_pos++];
    }
    /* Unbuffered: read one byte. */
    unsigned char ch;
    long n = sys_read(stream->fd, &ch, 1);
    if (n <= 0) { stream->eof_flag = (n == 0); return EOF; }
    return ch;
}

char *fgets(char *restrict s, int size, FILE *restrict stream) {
    if (!stream || !s || size <= 0) return (void *)0;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return (void *)0;
    s[i] = '\0';
    return s;
}

int ungetc(int c, FILE *stream) {
    if (!stream || c == EOF) return EOF;
    if (stream->buf && stream->buf_pos > 0) {
        stream->buf[--stream->buf_pos] = (unsigned char)c;
        stream->eof_flag = 0;
        return c;
    }
    return EOF;
}

/* =========================================================================
 * Formatted output — dispatches to vsnprintf + sys_write
 * ========================================================================= */

int vfprintf(FILE *restrict stream, const char *restrict fmt, va_list ap) {
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len < 0) return -1;

    const char *p = buf;
    int remaining = len;
    while (remaining > 0) {
        long n = sys_write(stream->fd, p, (size_t)remaining);
        if (n < 0) { stream->error_flag = 1; break; }
        p += n;
        remaining -= (int)n;
    }
    return len;
}

int fprintf(FILE *restrict stream, const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(stream, fmt, ap);
    va_end(ap);
    return ret;
}

int vprintf(const char *restrict fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return ret;
}

/* =========================================================================
 * Formatted input (stub — TODO)
 * ========================================================================= */

int fscanf(FILE *restrict stream, const char *restrict fmt, ...) {
    (void)stream; (void)fmt;
    return 0;
}

int scanf(const char *restrict fmt, ...) {
    (void)fmt;
    return 0;
}

/* =========================================================================
 * Error / state
 * ========================================================================= */

void perror(const char *s) {
    if (s && *s) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

int feof(FILE *stream) {
    return stream ? stream->eof_flag : 1;
}

int ferror(FILE *stream) {
    return stream ? stream->error_flag : 0;
}

int fileno(FILE *stream) {
    return stream ? stream->fd : -1;
}

void clearerr(FILE *stream) {
    if (stream) {
        stream->eof_flag   = 0;
        stream->error_flag = 0;
    }
}
