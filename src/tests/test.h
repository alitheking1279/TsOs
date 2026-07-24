#ifndef TESTS_TEST_H
#define TESTS_TEST_H

#include "../drivers/serial.h"

/* Raised from 256 → 512 to accommodate the full test suite.
 * At 256 the registry silently drops any test registered after slot 255. */
#define TEST_MAX 512

typedef void (*test_fn)(serial_dev_t *dev);

typedef struct {
    const char *name;
    test_fn     fn;
} test_entry_t;

/* test runner state (defined in test.c) */
extern int test_fail_flag;

void test_register(const char *name, test_fn fn);
int  test_run_all(serial_dev_t *dev);

/* ---- helpers ---- */
static inline void _print_int(serial_dev_t *dev, int n) {
    char buf[12];
    int i = 0;
    if (n == 0) { buf[i++] = '0'; }
    else {
        int neg = 0;
        if (n < 0) { neg = 1; n = -n; }
        while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
        if (neg) buf[i++] = '-';
    }
    buf[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t;
    }
    serial_write_string(dev, buf);
}

static inline void _print_hex(serial_dev_t *dev, uint64_t val) {
    static const char hex[] = "0123456789ABCDEF";
    serial_write_string(dev, "0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_write_char(dev, hex[(val >> i) & 0xF]);
}

/* ---- assertion macros ---- */

#define ASSERT_EQ(dev, a, b) do {                          \
    if ((a) != (b)) {                                      \
        serial_write_string(dev, "FAIL ");                  \
        serial_write_string(dev, __FILE__);                 \
        serial_write_string(dev, ":");                      \
        _print_int(dev, __LINE__);                          \
        serial_write_string(dev, "\r\n");                   \
        test_fail_flag = 1;                                 \
        return;                                             \
    }                                                       \
} while (0)

#define ASSERT_NEQ(dev, a, b) do {                         \
    if ((a) == (b)) {                                      \
        serial_write_string(dev, "FAIL ");                  \
        serial_write_string(dev, __FILE__);                 \
        serial_write_string(dev, ":");                      \
        _print_int(dev, __LINE__);                          \
        serial_write_string(dev, "\r\n");                   \
        test_fail_flag = 1;                                 \
        return;                                             \
    }                                                       \
} while (0)

/* ASSERT_EQ_VAL — like ASSERT_EQ but prints actual vs expected hex values. */
#define ASSERT_EQ_VAL(dev, a, b) do {                      \
    uint64_t _av = (uint64_t)(a);                          \
    uint64_t _bv = (uint64_t)(b);                          \
    if (_av != _bv) {                                      \
        serial_write_string(dev, "FAIL ");                  \
        serial_write_string(dev, __FILE__);                 \
        serial_write_string(dev, ":");                      \
        _print_int(dev, __LINE__);                          \
        serial_write_string(dev, " got=");                  \
        _print_hex(dev, _av);                               \
        serial_write_string(dev, " expected=");             \
        _print_hex(dev, _bv);                               \
        serial_write_string(dev, "\r\n");                   \
        test_fail_flag = 1;                                 \
        return;                                             \
    }                                                       \
} while (0)

/* ASSERT_NEQ_VAL — like ASSERT_NEQ but prints actual value on failure. */
#define ASSERT_NEQ_VAL(dev, a, b) do {                     \
    uint64_t _av = (uint64_t)(a);                          \
    uint64_t _bv = (uint64_t)(b);                          \
    if (_av == _bv) {                                      \
        serial_write_string(dev, "FAIL ");                  \
        serial_write_string(dev, __FILE__);                 \
        serial_write_string(dev, ":");                      \
        _print_int(dev, __LINE__);                          \
        serial_write_string(dev, " both=");                 \
        _print_hex(dev, _av);                               \
        serial_write_string(dev, "\r\n");                   \
        test_fail_flag = 1;                                 \
        return;                                             \
    }                                                       \
} while (0)

#define ASSERT_TRUE(dev, cond) do {                         \
    if (!(cond)) {                                          \
        serial_write_string(dev, "FAIL ");                  \
        serial_write_string(dev, __FILE__);                 \
        serial_write_string(dev, ":");                      \
        _print_int(dev, __LINE__);                          \
        serial_write_string(dev, "\r\n");                   \
        test_fail_flag = 1;                                 \
        return;                                             \
    }                                                       \
} while (0)

#define ASSERT_NOT_NULL(dev, ptr) do {                      \
    if ((ptr) == 0) {                                       \
        serial_write_string(dev, "FAIL ");                  \
        serial_write_string(dev, __FILE__);                 \
        serial_write_string(dev, ":");                      \
        _print_int(dev, __LINE__);                          \
        serial_write_string(dev, "\r\n");                   \
        test_fail_flag = 1;                                 \
        return;                                             \
    }                                                       \
} while (0)

#define ASSERT_NULL(dev, ptr) do {                          \
    if ((ptr) != 0) {                                       \
        serial_write_string(dev, "FAIL ");                  \
        serial_write_string(dev, __FILE__);                 \
        serial_write_string(dev, ":");                      \
        _print_int(dev, __LINE__);                          \
        serial_write_string(dev, "\r\n");                   \
        test_fail_flag = 1;                                 \
        return;                                             \
    }                                                       \
} while (0)

#endif /* TESTS_TEST_H */
