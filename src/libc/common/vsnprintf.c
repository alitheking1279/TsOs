/**
 * @file vsnprintf.c
 * @brief Pure formatted I/O engine — no syscalls, no I/O side effects.
 *
 * Contains vsnprintf, snprintf, sprintf, vsprintf, sscanf.
 * These are shared between kernel and userspace.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * vsnprintf — core formatted output engine
 *
 * Supports:
 *   Conversion specifiers: d, i, u, x, X, o, s, c, p, %, n
 *   Flags: -, +, space, 0, #
 *   Width: * or decimal
 *   Precision: .* or decimal
 *   Length modifiers: h, hh, l, ll, z, t
 * ========================================================================= */

static size_t vsnprintf_write(char *buf, size_t size, size_t pos, char c) {
    if (pos < size - 1)
        buf[pos] = c;
    return pos + 1;
}

static size_t vsnprintf_writes(char *buf, size_t size, size_t pos,
                                const char *s, int len) {
    for (int i = 0; i < len; i++)
        pos = vsnprintf_write(buf, size, pos, s[i]);
    return pos;
}

int vsnprintf(char *restrict buf, size_t size, const char *restrict fmt,
              va_list ap) {
    if (size == 0) return 0;
    if (size == (size_t)-1) size = (size_t)0x7FFFFFFFFFFFFFFF;

    size_t pos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            pos = vsnprintf_write(buf, size, pos, *fmt++);
            continue;
        }
        fmt++;

        int flag_minus = 0, flag_plus = 0, flag_space = 0;
        int flag_zero = 0, flag_hash = 0;
        for (;;) {
            switch (*fmt) {
            case '-':  flag_minus = 1;  fmt++; continue;
            case '+':  flag_plus = 1;   fmt++; continue;
            case ' ':  flag_space = 1;  fmt++; continue;
            case '0':  flag_zero = 1;   fmt++; continue;
            case '#':  flag_hash = 1;   fmt++; continue;
            default: goto done_flags;
            }
        }
    done_flags:

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flag_minus = 1; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }

        int mod_long = 0, mod_longlong = 0, mod_size = 0, mod_short = 0;
        int mod_ptrdiff = 0;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { mod_longlong = 1; fmt++; }
            else { mod_long = 1; }
        } else if (*fmt == 'z') {
            mod_size = 1; fmt++;
        } else if (*fmt == 't') {
            mod_ptrdiff = 1; fmt++;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') { mod_short = 2; fmt++; }
            else { mod_short = 1; }
        }

        char numbuf[32];
        int numlen = 0;
        int negative = 0;
        int is_unsigned = 0;
        uint64_t uval = 0;
        int64_t val = 0;
        const char *str = (void *)0;
        int strlen_val = 0;
        int base = 10;
        char conv = *fmt++;

        switch (conv) {
        case 'd': case 'i':
            if (mod_longlong) val = va_arg(ap, long long);
            else if (mod_long) val = va_arg(ap, long);
            else if (mod_size) val = (long)va_arg(ap, size_t);
            else if (mod_ptrdiff) val = (long)va_arg(ap, ptrdiff_t);
            else val = va_arg(ap, int);

            if (mod_short == 1) val = (short)val;
            else if (mod_short == 2) val = (signed char)val;

            if (val < 0) { negative = 1; uval = (uint64_t)(-(val + 1)) + 1; }
            else uval = (uint64_t)val;

            if (uval == 0) {
                numbuf[numlen++] = '0';
            } else {
                while (uval > 0) {
                    numbuf[numlen++] = '0' + (uval % 10);
                    uval /= 10;
                }
            }
            for (int i = 0; i < numlen / 2; i++) {
                char t = numbuf[i];
                numbuf[i] = numbuf[numlen - 1 - i];
                numbuf[numlen - 1 - i] = t;
            }
            base = 10;
            break;

        case 'u':
            is_unsigned = 1;
            goto handle_unsigned;

        case 'x':
        case 'X': {
            is_unsigned = 1;
        handle_unsigned:
            if (mod_longlong) uval = va_arg(ap, unsigned long long);
            else if (mod_long) uval = va_arg(ap, unsigned long);
            else if (mod_size) uval = va_arg(ap, size_t);
            else uval = va_arg(ap, unsigned int);

            if (mod_short == 1) uval = (unsigned short)uval;
            else if (mod_short == 2) uval = (unsigned char)uval;

            {
            const char *digits = (conv == 'X') ? "0123456789ABCDEF"
                                                : "0123456789abcdef";
            base = 16;
            if (uval == 0) {
                numbuf[numlen++] = '0';
            } else {
                while (uval > 0) {
                    numbuf[numlen++] = digits[uval % 16];
                    uval /= 16;
                }
            }
            for (int i = 0; i < numlen / 2; i++) {
                char t = numbuf[i];
                numbuf[i] = numbuf[numlen - 1 - i];
                numbuf[numlen - 1 - i] = t;
            }
            }
            break;
        }

        case 'o': {
            is_unsigned = 1;
            if (mod_longlong) uval = va_arg(ap, unsigned long long);
            else if (mod_long) uval = va_arg(ap, unsigned long);
            else uval = va_arg(ap, unsigned int);

            base = 8;
            if (uval == 0) {
                numbuf[numlen++] = '0';
            } else {
                while (uval > 0) {
                    numbuf[numlen++] = '0' + (uval % 8);
                    uval /= 8;
                }
            }
            for (int i = 0; i < numlen / 2; i++) {
                char t = numbuf[i];
                numbuf[i] = numbuf[numlen - 1 - i];
                numbuf[numlen - 1 - i] = t;
            }
            break;
        }

        case 's':
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            strlen_val = (int)strlen(str);
            break;

        case 'c':
            numbuf[0] = (char)va_arg(ap, int);
            numlen = 1;
            str = numbuf;
            strlen_val = 1;
            str = (void *)0;
            break;

        case 'p': {
            uint64_t ptrval = (uint64_t)(uintptr_t)va_arg(ap, void *);
            numbuf[0] = '0'; numbuf[1] = 'x';
            numlen = 2;
            {
            const char *digits = "0123456789abcdef";
            if (ptrval == 0) {
                numbuf[numlen++] = '0';
            } else {
                char tmp[16];
                int tlen = 0;
                while (ptrval > 0) {
                    tmp[tlen++] = digits[ptrval % 16];
                    ptrval /= 16;
                }
                for (int i = tlen - 1; i >= 0; i--)
                    numbuf[numlen++] = tmp[i];
            }
            }
            break;
        }

        case '%':
            pos = vsnprintf_write(buf, size, pos, '%');
            continue;

        case 'n': {
            int *p = va_arg(ap, int *);
            if (p) *p = (int)pos;
            continue;
        }

        default:
            pos = vsnprintf_write(buf, size, pos, '%');
            pos = vsnprintf_write(buf, size, pos, conv);
            continue;
        }

        int pad_char = flag_zero && !flag_minus ? '0' : ' ';
        int prefix_len = 0;
        char prefix[4] = {0};

        if (!str) str = numbuf;
        if (conv != 's' && conv != 'c' && conv != 'p') strlen_val = numlen;

        if (negative) { prefix[prefix_len++] = '-'; }
        else if (flag_plus && !is_unsigned) { prefix[prefix_len++] = '+'; }
        else if (flag_space && !is_unsigned) { prefix[prefix_len++] = ' '; }

        if (flag_hash && !is_unsigned && conv != 's' && conv != 'c') {
            if (base == 16) {
                prefix[prefix_len++] = '0';
                prefix[prefix_len++] = (conv == 'X') ? 'X' : 'x';
            } else if (base == 8) {
                prefix[prefix_len++] = '0';
            }
        }

        if (conv == 'p') {
            prefix[0] = '0'; prefix[1] = 'x'; prefix_len = 2;
        }

        if (prec >= 0 && conv != 's' && conv != 'c') {
            pad_char = ' ';
        }

        int content_len = prefix_len + strlen_val;
        int total_pad = width > content_len ? width - content_len : 0;

        if (!flag_minus) {
            while (total_pad-- > 0)
                pos = vsnprintf_write(buf, size, pos, pad_char);
        }

        for (int i = 0; i < prefix_len; i++)
            pos = vsnprintf_write(buf, size, pos, prefix[i]);

        if (prec > 0 && conv != 's' && conv != 'c') {
            int num_width = numlen;
            while (prec > num_width) {
                pos = vsnprintf_write(buf, size, pos, '0');
                prec--;
                if (total_pad > 0) total_pad--;
            }
        }

        if (conv == 's' && prec >= 0 && prec < strlen_val)
            strlen_val = prec;

        if (conv == 'c' && !str) {
            str = numbuf;
            strlen_val = 1;
        }

        if (str)
            pos = vsnprintf_writes(buf, size, pos, str, strlen_val);
        else
            pos = vsnprintf_writes(buf, size, pos, numbuf, numlen);

        while (total_pad-- > 0)
            pos = vsnprintf_write(buf, size, pos, ' ');
    }

    if (pos < size)
        buf[pos] = '\0';
    else if (size > 0)
        buf[size - 1] = '\0';

    return (int)pos;
}

int snprintf(char *restrict buf, size_t size, const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *restrict buf, const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}

int vsprintf(char *restrict buf, const char *restrict fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

/* =========================================================================
 * sscanf — minimal implementation
 *
 * Supports: %d, %u, %x, %s, %c, %n, %%, whitespace matching
 * ========================================================================= */

static int parse_int(const char **s, int base) {
    int result = 0;
    int neg = 0;
    while (**s == ' ' || **s == '\t' || **s == '\n') (*s)++;
    if (**s == '-') { neg = 1; (*s)++; }
    else if (**s == '+') { (*s)++; }
    while (**s) {
        int c = **s;
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        (*s)++;
    }
    return neg ? -result : result;
}

static unsigned int parse_uint(const char **s, int base) {
    unsigned int result = 0;
    while (**s == ' ' || **s == '\t' || **s == '\n') (*s)++;
    if (**s == '0' && (base == 0 || base == 16) && ((*s)[1] == 'x' || (*s)[1] == 'X')) {
        *s += 2;
        base = 16;
    }
    if (base == 0) {
        if (**s == '0') base = 8;
        else base = 10;
    }
    while (**s) {
        int c = **s;
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        (*s)++;
    }
    return result;
}

int sscanf(const char *restrict s, const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;

    while (*fmt && *s) {
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            fmt++;
            continue;
        }

        if (*fmt != '%') {
            if (*fmt != *s) break;
            fmt++;
            s++;
            continue;
        }
        fmt++;

        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            int val = parse_int(&s, 10);
            if (!suppress) { *va_arg(ap, int *) = val; count++; }
            break;
        }
        case 'u': {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            unsigned int val = parse_uint(&s, 10);
            if (!suppress) { *va_arg(ap, unsigned int *) = val; count++; }
            break;
        }
        case 'x': case 'X': {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            unsigned int val = parse_uint(&s, 16);
            if (!suppress) { *va_arg(ap, unsigned int *) = val; count++; }
            break;
        }
        case 's': {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            char *dest = suppress ? (void *)0 : va_arg(ap, char *);
            int n = 0;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
                if (width > 0 && n >= width) break;
                if (dest) *dest++ = *s;
                n++;
                s++;
            }
            if (dest) *dest = '\0';
            if (n > 0) count++;
            break;
        }
        case 'c': {
            char *dest = suppress ? (void *)0 : va_arg(ap, char *);
            if (dest) *dest = *s;
            s++;
            if (!suppress) count++;
            break;
        }
        case 'n': {
            if (!suppress) *va_arg(ap, int *) = (int)(s - fmt);
            break;
        }
        case '%':
            if (*s != '%') goto done;
            s++;
            break;
        default:
            goto done;
        }
        fmt++;
    }

done:
    va_end(ap);
    return count;
}
