/**
 * @file ctype.h
 * @brief Character classification and conversion — freestanding libc.
 *
 * C11 §7.4. All functions accept int values representable as unsigned char
 * or EOF (-1). Passing any other value is undefined behavior.
 */

#ifndef _LIBC_CTYPE_H
#define _LIBC_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- Classification (7.4.1) --- */
int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);

/* --- Conversion (7.4.2) --- */
int tolower(int c);
int toupper(int c);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_CTYPE_H */
