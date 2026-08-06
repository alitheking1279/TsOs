/**
 * @file stdlib.c
 * @brief Pure freestanding <stdlib.h> functions.
 *
 * Shared between kernel and userspace — no syscalls, no kernel headers.
 * atoi, strtol, strtoul, strtod, strtof, rand, qsort, bsearch, div, labs, etc.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* =========================================================================
 * String-to-number (7.22.1)
 * ========================================================================= */

int atoi(const char *s) {
    return (int)strtol(s, (void *)0, 10);
}

long atol(const char *s) {
    return strtol(s, (void *)0, 10);
}

long long atoll(const char *s) {
    return (long long)strtol(s, (void *)0, 10);
}

long strtol(const char *restrict s, char **restrict endp, int base) {
    const char *p = s;
    int negative = 0;
    int overflow = 0;
    unsigned long cutoff;
    unsigned long cutoff_rem;

    while (*p == ' ' || *p == '\t' || *p == '\n' ||
           *p == '\r' || *p == '\f' || *p == '\v')
        p++;

    if (*p == '-') { negative = 1; p++; }
    else if (*p == '+') { p++; }

    if (base == 0) {
        if (*p == '0') {
            if (p[1] == 'x' || p[1] == 'X') { base = 16; p += 2; }
            else { base = 8; p++; }
        } else {
            base = 10;
        }
    } else if (base == 16 && *p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    if (base < 2 || base > 36) {
        if (endp) *endp = (char *)s;
        return 0;
    }

    if (negative) {
        cutoff = (unsigned long)(-(LONG_MIN + 1)) / (unsigned long)base;
        cutoff_rem = (unsigned long)(-(LONG_MIN + 1)) % (unsigned long)base;
    } else {
        cutoff = (unsigned long)LONG_MAX / (unsigned long)base;
        cutoff_rem = (unsigned long)LONG_MAX % (unsigned long)base;
    }

    /* Convert digits */
    const char *start = p;
    unsigned long accum = 0;
    for (;;) {
        int digit;
        int c = *p;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else break;

        if (digit >= base) break;

        if (accum > cutoff || (accum == cutoff && (unsigned)digit > cutoff_rem))
            overflow = 1;

        accum = accum * (unsigned long)base + (unsigned long)digit;
        p++;
    }

    if (p == start) {
        if (endp) *endp = (char *)s;
        return 0;
    }

    if (endp) *endp = (char *)p;

    if (overflow) {
        if (negative)
            return LONG_MIN;
        else
            return LONG_MAX;
    }

    return negative ? -(long)accum : (long)accum;
}

unsigned long strtoul(const char *restrict s, char **restrict endp, int base) {
    const char *p = s;
    unsigned long result = 0;
    int overflow = 0;
    unsigned long cutoff;
    unsigned long cutoff_rem;

    while (*p == ' ' || *p == '\t' || *p == '\n' ||
           *p == '\r' || *p == '\f' || *p == '\v')
        p++;

    int negative = 0;
    if (*p == '-') { negative = 1; p++; }
    else if (*p == '+') { p++; }

    if (base == 0) {
        if (*p == '0') {
            if (p[1] == 'x' || p[1] == 'X') { base = 16; p += 2; }
            else { base = 8; p++; }
        } else {
            base = 10;
        }
    } else if (base == 16 && *p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    if (base < 2 || base > 36) {
        if (endp) *endp = (char *)s;
        return 0;
    }

    cutoff = ULONG_MAX / (unsigned long)base;
    cutoff_rem = ULONG_MAX % (unsigned long)base;

    const char *start = p;
    for (;;) {
        int digit;
        int c = *p;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else break;

        if (digit >= base) break;

        if (result > cutoff || (result == cutoff && (unsigned)digit > cutoff_rem))
            overflow = 1;

        result = result * (unsigned long)base + (unsigned long)digit;
        p++;
    }

    if (p == start) {
        if (endp) *endp = (char *)s;
        return 0;
    }

    if (endp) *endp = (char *)p;

    if (overflow) {
        return ULONG_MAX;
    }

    return negative ? -result : result;
}

/* =========================================================================
 * Floating-point string-to-number
 * NOTE: strtod/strtof cannot be implemented here because -mgeneral-regs-only
 * disables SSE and double cannot be returned via XMM registers.
 * DOOM uses integer math only. Keep declarations in stdlib.h for API
 * completeness; do not define them here.
 * ========================================================================= */

/* =========================================================================
 * Pseudo-random (7.22.2) — Linear congruential generator
 * ========================================================================= */

static unsigned int rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 16) & 0x7FFF);
}

void srand(unsigned int seed) {
    rand_state = seed;
}

/* =========================================================================
 * Integer arithmetic (7.22.6)
 * ========================================================================= */

int abs(int x) {
    return x < 0 ? -x : x;
}

long labs(long x) {
    return x < 0 ? -x : x;
}

long long llabs(long long x) {
    return x < 0 ? -x : x;
}

div_t div(int numer, int denom) {
    div_t result;
    result.quot = numer / denom;
    result.rem = numer - result.quot * denom;
    return result;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t result;
    result.quot = numer / denom;
    result.rem = numer - result.quot * denom;
    return result;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t result;
    result.quot = numer / denom;
    result.rem = numer - result.quot * denom;
    return result;
}

/* =========================================================================
 * qsort — Quicksort with median-of-3 pivot
 * ========================================================================= */

static void swap_bytes(char *a, char *b, size_t size) {
    while (size--) {
        char tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    }
}

static char *median3(char *a, char *b, char *c,
                     int (*compar)(const void *, const void *)) {
    if (compar(a, b) > 0) swap_bytes(a, b, sizeof(void *));
    if (compar(a, c) > 0) swap_bytes(a, c, sizeof(void *));
    if (compar(b, c) > 0) swap_bytes(b, c, sizeof(void *));
    return b;
}

static void qsort_impl(char *lo, char *hi, size_t size,
                        int (*compar)(const void *, const void *)) {
    while (lo < hi) {
        char *pivot = median3(lo, lo + size * ((hi - lo) / size / 2), hi - size, compar);

        swap_bytes(lo, pivot, size);

        char *i = lo + size;
        char *j = hi - size;

        for (;;) {
            while (i <= j && compar(i, lo) <= 0) i += size;
            while (j >= i && compar(j, lo) > 0) j -= size;
            if (i >= j) break;
            swap_bytes(i, j, size);
        }

        swap_bytes(lo, j, size);

        if ((size_t)(j - lo) < (size_t)(hi - (j + size))) {
            if (lo < j) qsort_impl(lo, j, size, compar);
            lo = j + size;
        } else {
            if (j + size < hi) qsort_impl(j + size, hi, size, compar);
            hi = j;
        }
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0) return;
    qsort_impl((char *)base, (char *)base + nmemb * size, size, compar);
}

/* =========================================================================
 * bsearch — Binary search
 * ========================================================================= */

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    while (nmemb > 0) {
        size_t mid = nmemb / 2;
        const void *p = (const char *)base + mid * size;
        int cmp = compar(key, p);
        if (cmp == 0) return (void *)p;
        if (cmp < 0) {
            nmemb = mid;
        } else {
            base = (const char *)p + size;
            nmemb -= mid + 1;
        }
    }
    return (void *)0;
}
