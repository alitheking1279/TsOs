/**
 * @file strings.h
 * @brief String operations — freestanding libc.
 *
 * POSIX.1-2008 §4.2. Case-insensitive string functions.
 * Actual implementations live in common/string.c (shared with <string.h>).
 */

#ifndef _LIBC_STRINGS_H
#define _LIBC_STRINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Case-insensitive comparison (POSIX.1-2008 §4.2) --- */
int  strcasecmp(const char *s1, const char *s2);
int  strncasecmp(const char *s1, const char *s2, size_t n);

/* --- Miscellaneous --- */
char *index(const char *s, int c);
char *rindex(const char *s, int c);

/* --- Legacy bzero/bcopy (BSD) --- */
void bzero(void *s, size_t n);
void bcopy(const void *src, void *dest, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_STRINGS_H */
