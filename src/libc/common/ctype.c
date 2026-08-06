/**
 * @file ctype.c
 * @brief Complete <ctype.h> implementation — freestanding libc.
 *
 * All functions accept int values representable as unsigned char or EOF (-1).
 * Implements C11 §7.4 (all ctype.h functions).
 */

#include <ctype.h>

/* =========================================================================
 * Classification (7.4.1)
 *
 * All functions cast to (unsigned char) before testing per C11 §7.4p1.
 * ========================================================================= */

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isalpha(int c) {
    return isupper(c) || islower(c);
}

int isblank(int c) {
    return c == ' ' || c == '\t';
}

int iscntrl(int c) {
    return (unsigned char)c < 0x20 || c == 0x7F;
}

int isdigit(int c) {
    return (unsigned char)c >= '0' && (unsigned char)c <= '9';
}

int isgraph(int c) {
    return isprint(c) && !((unsigned char)c == ' ');
}

int islower(int c) {
    return (unsigned char)c >= 'a' && (unsigned char)c <= 'z';
}

int isprint(int c) {
    return (unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E;
}

int ispunct(int c) {
    return isprint(c) && !isalnum(c) && !((unsigned char)c == ' ');
}

int isspace(int c) {
    return c == ' '  || c == '\t' || c == '\n' ||
           c == '\v' || c == '\f' || c == '\r';
}

int isupper(int c) {
    return (unsigned char)c >= 'A' && (unsigned char)c <= 'Z';
}

int isxdigit(int c) {
    return isdigit(c) ||
           ((unsigned char)c >= 'a' && (unsigned char)c <= 'f') ||
           ((unsigned char)c >= 'A' && (unsigned char)c <= 'F');
}

/* =========================================================================
 * Conversion (7.4.2)
 * ========================================================================= */

int tolower(int c) {
    if (isupper(c))
        return c + ('a' - 'A');
    return c;
}

int toupper(int c) {
    if (islower(c))
        return c - ('a' - 'A');
    return c;
}
