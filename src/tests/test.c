#include "test.h"

static test_entry_t tests[TEST_MAX];
static int test_count = 0;

int test_fail_flag = 0;

void test_register(const char *name, test_fn fn) {
    if (test_count < TEST_MAX) {
        tests[test_count].name = name;
        tests[test_count].fn   = fn;
        test_count++;
    }
}

int test_run_all(serial_dev_t *dev) {
    int passed = 0;
    int failed = 0;

    serial_write_string(dev, "--- Running Tests ---\r\n");

    for (int i = 0; i < test_count; i++) {
        serial_write_string(dev, tests[i].name);
        serial_write_string(dev, " ... ");

        test_fail_flag = 0;
        tests[i].fn(dev);

        if (test_fail_flag) {
            failed++;
        } else {
            serial_write_string(dev, "OK\r\n");
            passed++;
        }
    }

    serial_write_string(dev, "--- Results: ");
    _print_int(dev, passed);
    serial_write_string(dev, "/");
    _print_int(dev, test_count);
    serial_write_string(dev, " PASSED ---\r\n");

    if (failed > 0) {
        serial_write_string(dev, "FAILED: ");
        _print_int(dev, failed);
        serial_write_string(dev, " test(s) failed\r\n");
    }

    return failed == 0 ? 0 : 1;
}
