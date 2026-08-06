/**
 * @file assert.h
 * @brief Debugging macro — freestanding libc.
 *
 * C11 §7.2. assert() is a no-op when NDEBUG is defined.
 */

#ifndef _LIBC_ASSERT_H
#define _LIBC_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
void __assert_fail(const char *expr, const char *file, int line);
#define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__))
#endif

/* C11 _Static_assert (available since C11, no library support needed) */
/* Provided by compiler, not libc — but we expose the macro. */

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_ASSERT_H */
