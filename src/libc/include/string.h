/**
 * @file string.h
 * @brief String and memory manipulation — freestanding libc.
 *
 * C11 §7.24. All functions are pure (no OS dependencies, no syscalls).
 */

#ifndef _LIBC_STRING_H
#define _LIBC_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Memory (7.24.1) --- */
void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);

/* --- String (7.24.2) --- */
size_t strlen(const char *s);
char  *strcpy(char *restrict dest, const char *restrict src);
char  *strncpy(char *restrict dest, const char *restrict src, size_t n);
char  *strcat(char *restrict dest, const char *restrict src);
char  *strncat(char *restrict dest, const char *restrict src, size_t n);

/* --- Comparison (7.24.4) --- */
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

/* --- Search (7.24.5) --- */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

/* --- Tokenization (7.24.5) --- */
char *strtok(char *restrict str, const char *restrict delim);

/* --- Misc (7.24.6) --- */
char *strdup(const char *s);
char *strerror(int errnum);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_STRING_H */
