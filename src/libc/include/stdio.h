/**
 * @file stdio.h
 * @brief Input/output — freestanding libc.
 *
 * C11 §7.21. Buffered I/O and formatted output.
 * Kernel build: printf→serial, FILE is minimal.
 * Userspace build: full FILE with fd-backed buffering.
 */

#ifndef _LIBC_STDIO_H
#define _LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NULL   ((void *)0)
#define EOF    (-1)
#define BUFSIZ 4096

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _IONBF 0
#define _IOLBF 1
#define _IOFBF 2

#define FOPEN_MAX 32
#define FILENAME_MAX 4096

/* --- FILE type --- */
struct _FILE {
    int fd;
    int eof_flag;
    int error_flag;
    unsigned char *buf;
    size_t buf_size;
    size_t buf_pos;
    size_t buf_len;
    int buf_mode;
    int mode_flags;
};
typedef struct _FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* --- File operations (7.21.3) --- */
FILE *fopen(const char *restrict path, const char *restrict mode);
int   fclose(FILE *stream);
size_t fread(void *restrict ptr, size_t size, size_t nmemb,
             FILE *restrict stream);
size_t fwrite(const void *restrict ptr, size_t size, size_t nmemb,
              FILE *restrict stream);
int   fseek(FILE *stream, long offset, int whence);
long  ftell(FILE *stream);
void  rewind(FILE *stream);
int   fflush(FILE *stream);
int   rename(const char *oldpath, const char *newpath);

/* --- Unformatted input (7.21.6) --- */
int  fgetc(FILE *stream);
char *fgets(char *restrict s, int size, FILE *restrict stream);
int  ungetc(int c, FILE *stream);

/* --- Unformatted output (7.21.6) --- */
int  fputc(int c, FILE *stream);
int  fputs(const char *restrict s, FILE *restrict stream);
int  puts(const char *s);
int  putchar(int c);

/* --- Formatted output (7.21.6) --- */
int  fprintf(FILE *restrict stream, const char *restrict fmt, ...);
int  printf(const char *restrict fmt, ...);
int  sprintf(char *restrict buf, const char *restrict fmt, ...);
int  snprintf(char *restrict buf, size_t size, const char *restrict fmt, ...);
int  vfprintf(FILE *restrict stream, const char *restrict fmt, va_list ap);
int  vprintf(const char *restrict fmt, va_list ap);
int  vsprintf(char *restrict buf, const char *restrict fmt, va_list ap);
int  vsnprintf(char *restrict buf, size_t size, const char *restrict fmt,
               va_list ap);

/* --- Formatted input (7.21.6) --- */
int  fscanf(FILE *restrict stream, const char *restrict fmt, ...);
int  scanf(const char *restrict fmt, ...);
int  sscanf(const char *restrict s, const char *restrict fmt, ...);

/* --- Error handling (7.21.10) --- */
void perror(const char *s);

/* --- Stream state (7.21.9) --- */
int  feof(FILE *stream);
int  ferror(FILE *stream);
int  fileno(FILE *stream);
void clearerr(FILE *stream);

/* --- Buffering (7.21.5) --- */
int setvbuf(FILE *restrict stream, char *restrict buf, int mode, size_t size);

/* --- Kernel extension --- */
#ifdef __KERNEL__
void stdio_set_serial(void *dev);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_STDIO_H */
