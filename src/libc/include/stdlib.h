/**
 * @file stdlib.h
 * @brief General utilities — freestanding libc.
 *
 * C11 §7.22. Memory allocation, conversion, and utility functions.
 * Kernel build wraps kmalloc/kfree; userspace build uses syscalls.
 */

#ifndef _LIBC_STDLIB_H
#define _LIBC_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NULL   ((void *)0)
#define RAND_MAX 2147483647

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* --- Numeric conversion (7.22.1) --- */
int           atoi(const char *s);
long          atol(const char *s);
long long     atoll(const char *s);
long          strtol(const char *restrict s, char **restrict endp, int base);
unsigned long strtoul(const char *restrict s, char **restrict endp, int base);
double        strtod(const char *restrict s, char **restrict endp);
float         strtof(const char *restrict s, char **restrict endp);

/* --- Pseudo-random (7.22.2) --- */
int  rand(void);
void srand(unsigned int seed);

/* --- Memory allocation (7.22.3) --- */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* --- Integer arithmetic (7.22.6) --- */
int abs(int x);
long labs(long x);
long long llabs(long long x);

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

div_t   div(int numer, int denom);
ldiv_t  ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

/* --- Sorting and searching (7.22.5) --- */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* --- Process control (7.22.4) --- */
void exit(int status) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int  atexit(void (*func)(void));

/* --- Environment (7.22.4.6) --- */
char *getenv(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_STDLIB_H */
