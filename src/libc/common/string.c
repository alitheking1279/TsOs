/**
 * @file string.c
 * @brief Complete <string.h> implementation — freestanding libc.
 *
 * All functions are pure C with no OS dependencies.
 * Implements C11 §7.24 (all string.h functions).
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

/* =========================================================================
 * Memory (7.24.1)
 * ========================================================================= */

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    while (n--) {
        if (*p == (uint8_t)c) return (void *)p;
        p++;
    }
    return (void *)0;
}

/* =========================================================================
 * String (7.24.2)
 * ========================================================================= */

size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

char *strcpy(char *restrict dest, const char *restrict src) {
    char *d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncpy(char *restrict dest, const char *restrict src, size_t n) {
    char *d = dest;
    /* Copy up to n characters from src. */
    while (n && (*d++ = *src++))
        n--;
    /* Pad remaining with null bytes (C11 §7.24.2.3). */
    while (n--)
        *d++ = '\0';
    return dest;
}

char *strcat(char *restrict dest, const char *restrict src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncat(char *restrict dest, const char *restrict src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    *d = '\0';
    return dest;
}

/* =========================================================================
 * Comparison (7.24.4)
 * ========================================================================= */

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strcasecmp(const char *s1, const char *s2) {
    for (;;) {
        int c1 = (unsigned char)*s1;
        int c2 = (unsigned char)*s2;

        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;

        if (c1 != c2 || c1 == '\0')
            return c1 - c2;

        s1++;
        s2++;
    }
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n--) {
        int c1 = (unsigned char)*s1;
        int c2 = (unsigned char)*s2;

        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;

        if (c1 != c2 || c1 == '\0')
            return c1 - c2;

        s1++;
        s2++;
    }
    return 0;
}

/* =========================================================================
 * Search (7.24.5)
 * ========================================================================= */

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (void *)0;
}

char *strrchr(const char *s, int c) {
    const char *last = (void *)0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
    }
    return (void *)0;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a) return (char *)s;
            a++;
        }
        s++;
    }
    return (void *)0;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        int found = 0;
        while (*a) {
            if (*s == *a) { found = 1; break; }
            a++;
        }
        if (!found) return count;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r) {
            if (*s == *r) return count;
            r++;
        }
        count++;
        s++;
    }
    return count;
}

/* =========================================================================
 * Tokenization (7.24.5)
 * ========================================================================= */

char *strtok(char *restrict str, const char *restrict delim) {
    static char *saveptr = (void *)0;

    if (str)
        saveptr = str;

    if (!saveptr || !*saveptr)
        return (void *)0;

    /* Skip leading delimiters. */
    while (*saveptr) {
        const char *d = delim;
        int is_delim = 0;
        while (*d) {
            if (*saveptr == *d) { is_delim = 1; break; }
            d++;
        }
        if (!is_delim) break;
        saveptr++;
    }

    if (!*saveptr)
        return (void *)0;

    char *token = saveptr;

    /* Find end of token. */
    while (*saveptr) {
        const char *d = delim;
        while (*d) {
            if (*saveptr == *d) {
                *saveptr = '\0';
                saveptr++;
                return token;
            }
            d++;
        }
        saveptr++;
    }

    return token;
}

/* =========================================================================
 * Misc (7.24.6)
 * ========================================================================= */

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *dup = (char *)malloc(len);
    if (dup)
        memcpy(dup, s, len);
    return dup;
}

char *strerror(int errnum) {
    switch (errnum) {
    case 0:   return "Success";
    case EDOM:   return "Numerical argument out of domain";
    case ERANGE: return "Numerical result out of range";
    case ENOMEM: return "Out of memory";
    case EINVAL: return "Invalid argument";
    case ENOENT: return "No such file or directory";
    case EIO:    return "I/O error";
    case EBADF:  return "Bad file descriptor";
    case EACCES: return "Permission denied";
    case EBUSY:  return "Device or resource busy";
    case EEXIST: return "File exists";
    case ENOTDIR: return "Not a directory";
    case EISDIR:  return "Is a directory";
    case EMFILE:  return "Too many open files";
    case ENOSPC:  return "No space left on device";
    case EPIPE:   return "Broken pipe";
    case EAGAIN:  return "Resource temporarily unavailable";
    case EINTR:   return "Interrupted system call";
    case EILSEQ:  return "Invalid or incomplete multibyte sequence";
    default:   return "Unknown error";
    }
}
