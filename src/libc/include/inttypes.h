/**
 * @file inttypes.h
 * @brief Integer format macros — freestanding libc.
 *
 * C11 §7.8. Format macros for printf/scanf of fixed-width integers.
 * Only x86_64 (LP64) model: long = 64-bit, int = 32-bit.
 */

#ifndef _LIBC_INTTYPES_H
#define _LIBC_INTTYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Signed integer format macros --- */
#define PRId8    "d"
#define PRId16   "d"
#define PRId32   "d"
#define PRId64   "ld"

#define PRIdLEAST8   "d"
#define PRIdLEAST16  "d"
#define PRIdLEAST32  "d"
#define PRIdLEAST64  "ld"

#define PRIdFAST8    "d"
#define PRIdFAST16   "ld"
#define PRIdFAST32   "ld"
#define PRIdFAST64   "ld"

#define PRIdPTR     "ld"
#define PRIdMAX     "ld"

/* --- Unsigned integer format macros --- */
#define PRIu8    "u"
#define PRIu16   "u"
#define PRIu32   "u"
#define PRIu64   "lu"

#define PRIuLEAST8   "u"
#define PRIuLEAST16  "u"
#define PRIuLEAST32  "u"
#define PRIuLEAST64  "lu"

#define PRIuFAST8    "u"
#define PRIuFAST16   "lu"
#define PRIuFAST32   "lu"
#define PRIuFAST64   "lu"

#define PRIuPTR     "lu"
#define PRIuMAX     "lu"

/* --- Octal format macros --- */
#define PRIo8    "o"
#define PRIo16   "o"
#define PRIo32   "o"
#define PRIo64   "lo"

#define PRIoPTR     "lo"
#define PRIoMAX     "lo"

/* --- Hexadecimal format macros (lowercase) --- */
#define PRIx8    "x"
#define PRIx16   "x"
#define PRIx32   "x"
#define PRIx64   "lx"

#define PRIxPTR     "lx"
#define PRIxMAX     "lx"

/* --- Hexadecimal format macros (uppercase) --- */
#define PRIX8    "X"
#define PRIX16   "X"
#define PRIX32   "X"
#define PRIX64   "lX"

#define PRIXPTR     "lX"
#define PRIXMAX     "lX"

/* --- scanf format macros --- */
#define SCNd8    "hhd"
#define SCNd16   "hd"
#define SCNd32   "d"
#define SCNd64   "ld"

#define SCNu8    "hhu"
#define SCNu16   "hu"
#define SCNu32   "u"
#define SCNu64   "lu"

#define SCNx8    "hhx"
#define SCNx16   "hx"
#define SCNx32   "x"
#define SCNx64   "lx"

/* --- intmax_t format macros --- */
#define PRIdMAX    "ld"
#define PRIiMAX    "li"
#define PRIoMAX    "lo"
#define PRIuMAX    "lu"
#define PRIxMAX    "lx"
#define PRIXMAX    "lX"

/* --- imaxdiv_t --- */
typedef struct {
    long quot;
    long rem;
} imaxdiv_t;

imaxdiv_t imaxdiv(long numer, long denom);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_INTTYPES_H */
