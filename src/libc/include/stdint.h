/**
 * @file stdint.h
 * @brief Forward to GCC compiler-provided stdint.h.
 *
 * Freestanding GCC provides this header directly.
 * This file exists so -Isrc/libc/include doesn't shadow it.
 */
#ifndef _LIBC_STDINT_H
#define _LIBC_STDINT_H
#include_next <stdint.h>
#endif
