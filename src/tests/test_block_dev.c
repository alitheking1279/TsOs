/**
 * @file test_block_dev.c
 * @brief Block device abstraction layer test suite.
 *
 * Tests the block_dev API using a mock (RAM-backed) block device so
 * we can verify the abstraction layer independently of hardware.
 *
 * Test groups:
 *   Group A — Registration
 *   Group B — Mock device I/O
 *   Group C — Multi-sector operations
 *   Group D — Edge cases and error handling
 */

#include "test.h"
#include "../fs/block_dev.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Mock Device — backed by a RAM buffer
 * ========================================================================= */

/** Size of the mock device in sectors. */
#define MOCK_SECTORS        16
#define MOCK_SECTOR_SIZE    512

/** RAM backing store for the mock device. */
static uint8_t mock_storage[MOCK_SECTORS * MOCK_SECTOR_SIZE];

/** Global read/write counters for verification. */
static uint32_t mock_read_count;
static uint32_t mock_write_count;

/** Simulated error: when set, the mock device returns this error. */
static int mock_simulated_error;

/**
 * @brief Mock read_sector — reads from the RAM backing store.
 */
static block_status_t mock_read(uint8_t dev_id, uint32_t lba, void *buf) {
    (void)dev_id;
    if (mock_simulated_error) return (block_status_t)mock_simulated_error;
    if (lba >= MOCK_SECTORS) return BLOCK_ERR_INVALID;
    memcpy(buf, &mock_storage[lba * MOCK_SECTOR_SIZE], MOCK_SECTOR_SIZE);
    mock_read_count++;
    return BLOCK_OK;
}

/**
 * @brief Mock write_sector — writes to the RAM backing store.
 */
static block_status_t mock_write(uint8_t dev_id, uint32_t lba, const void *buf) {
    (void)dev_id;
    if (mock_simulated_error) return (block_status_t)mock_simulated_error;
    if (lba >= MOCK_SECTORS) return BLOCK_ERR_INVALID;
    memcpy(&mock_storage[lba * MOCK_SECTOR_SIZE], buf, MOCK_SECTOR_SIZE);
    mock_write_count++;
    return BLOCK_OK;
}

/**
 * @brief Set up the mock device — resets storage and counters.
 */
static void mock_reset(void) {
    memset(mock_storage, 0, sizeof(mock_storage));
    mock_read_count = 0;
    mock_write_count = 0;
    mock_simulated_error = 0;
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void t_hex(serial_dev_t *dev, uint32_t val) {
    static const char h[] = "0123456789ABCDEF";
    serial_write_string(dev, "0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_write_char(dev, h[(val >> i) & 0xF]);
}

static void t_log(serial_dev_t *dev, const char *label, uint32_t val) {
    serial_write_string(dev, "[BDEV TEST]   ");
    serial_write_string(dev, label);
    serial_write_string(dev, " = ");
    t_hex(dev, val);
    serial_write_string(dev, "\r\n");
}

/* =========================================================================
 * Helper: Unregister all devices for test isolation
 * ========================================================================= */

static void unregister_all(void) {
    for (uint8_t i = 0; i < BLOCK_DEV_MAX; i++) {
        block_dev_unregister(i);
    }
}

/* =========================================================================
 * Group A — Registration
 * ========================================================================= */

/** A1: Register a mock device and verify the returned ID is 0. */
static void test_block_dev_register_first(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock_dev = {
        .read_sector = mock_read,
        .write_sector = mock_write,
        .sector_size = MOCK_SECTOR_SIZE,
        .total_sectors = MOCK_SECTORS,
    };

    uint8_t dev_id = 0xFF;
    block_status_t st = block_dev_register(&mock_dev, &dev_id);
    t_log(dev, "register_first: status", (uint32_t)(-st));
    t_log(dev, "register_first: dev_id", (uint32_t)dev_id);

    ASSERT_EQ(dev, (int)st, (int)BLOCK_OK);
    ASSERT_EQ(dev, dev_id, 0);
}

/** A2: Register multiple devices, verify sequential IDs. */
static void test_block_dev_register_multiple(serial_dev_t *dev) {
    unregister_all();

    block_device_t mock_a = { .read_sector = mock_read, .write_sector = mock_write,
                              .sector_size = 512, .total_sectors = 16 };
    block_device_t mock_b = { .read_sector = mock_read, .write_sector = mock_write,
                              .sector_size = 512, .total_sectors = 16 };

    uint8_t id_a, id_b;
    block_status_t st_a = block_dev_register(&mock_a, &id_a);
    block_status_t st_b = block_dev_register(&mock_b, &id_b);

    ASSERT_EQ(dev, (int)st_a, (int)BLOCK_OK);
    ASSERT_EQ(dev, (int)st_b, (int)BLOCK_OK);
    /* IDs should be different. */
    ASSERT_TRUE(dev, id_a != id_b);
    t_log(dev, "register_multi: id_a", (uint32_t)id_a);
    t_log(dev, "register_multi: id_b", (uint32_t)id_b);
}

/** A3: Register with NULL dev returns error. */
static void test_block_dev_register_null(serial_dev_t *dev) {
    uint8_t id;
    block_status_t st = block_dev_register(NULL, &id);
    t_log(dev, "register_null", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NULL_DEV);
}

/** A4: Register with NULL id pointer returns error. */
static void test_block_dev_register_null_id(serial_dev_t *dev) {
    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = 512, .total_sectors = 16 };
    block_status_t st = block_dev_register(&mock, NULL);
    t_log(dev, "register_null_id", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NULL_BUF);
}

/** A5: Register with NULL function pointers returns error. */
static void test_block_dev_register_null_fns(serial_dev_t *dev) {
    block_device_t bad_dev = { .read_sector = NULL, .write_sector = NULL,
                               .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_status_t st = block_dev_register(&bad_dev, &id);
    t_log(dev, "register_null_fns", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NULL_DEV);
}

/** A6: Register with only read_sector NULL. */
static void test_block_dev_register_null_read_fn(serial_dev_t *dev) {
    block_device_t bad_dev = { .read_sector = NULL, .write_sector = mock_write,
                               .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_status_t st = block_dev_register(&bad_dev, &id);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NULL_DEV);
}

/** A7: Get a registered device returns non-NULL. */
static void test_block_dev_get_present(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = MOCK_SECTOR_SIZE, .total_sectors = MOCK_SECTORS };
    uint8_t id;
    block_dev_register(&mock, &id);

    block_device_t *found = block_dev_get(id);
    ASSERT_NOT_NULL(dev, found);
    ASSERT_TRUE(dev, found->present);
}

/** A8: Get an unregistered device ID returns NULL. */
static void test_block_dev_get_absent(serial_dev_t *dev) {
    unregister_all();
    block_device_t *found = block_dev_get(0);
    ASSERT_NULL(dev, found);
}

/** A9: Get with out-of-range ID returns NULL. */
static void test_block_dev_get_out_of_range(serial_dev_t *dev) {
    block_device_t *found = block_dev_get(BLOCK_DEV_MAX + 5);
    ASSERT_NULL(dev, found);
}

/** A10: Unregister a device, verify get returns NULL. */
static void test_block_dev_unregister_works(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_dev_register(&mock, &id);

    block_status_t st = block_dev_unregister(id);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_OK);

    block_device_t *found = block_dev_get(id);
    ASSERT_NULL(dev, found);
}

/** A11: Unregister an unregistered device returns error. */
static void test_block_dev_unregister_not_found(serial_dev_t *dev) {
    unregister_all();
    block_status_t st = block_dev_unregister(3);
    t_log(dev, "unreg_not_found", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NOT_FOUND);
}

/* =========================================================================
 * Group B — Mock Device I/O
 * ========================================================================= */

/** B1: Write a sector through block_dev_write, read it back. */
static void test_block_dev_write_read_single(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = MOCK_SECTOR_SIZE, .total_sectors = MOCK_SECTORS };
    uint8_t id;
    block_dev_register(&mock, &id);

    uint8_t write_buf[MOCK_SECTOR_SIZE];
    uint8_t read_buf[MOCK_SECTOR_SIZE];

    /* Fill with pattern. */
    for (int i = 0; i < MOCK_SECTOR_SIZE; i++)
        write_buf[i] = (uint8_t)((i * 3 + 0xAA) & 0xFF);

    block_status_t st = block_dev_write(id, 0, 1, write_buf);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_OK);

    memset(read_buf, 0, sizeof(read_buf));
    st = block_dev_read(id, 0, 1, read_buf);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_OK);

    /* Verify. */
    for (int i = 0; i < MOCK_SECTOR_SIZE; i++) {
        if (read_buf[i] != write_buf[i]) {
            serial_write_string(dev, "[BDEV TEST]   FAIL: byte ");
            t_hex(dev, (uint32_t)i);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
}

/** B2: Verify read/write counters increment correctly. */
static void test_block_dev_counter_increment(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = MOCK_SECTOR_SIZE, .total_sectors = MOCK_SECTORS };
    uint8_t id;
    block_dev_register(&mock, &id);

    uint8_t buf[MOCK_SECTOR_SIZE];
    memset(buf, 0x42, sizeof(buf));

    block_dev_write(id, 5, 1, buf);
    block_dev_write(id, 6, 1, buf);
    block_dev_read(id, 5, 1, buf);
    block_dev_read(id, 7, 1, buf);

    t_log(dev, "counter: reads",  (uint32_t)mock_read_count);
    t_log(dev, "counter: writes", (uint32_t)mock_write_count);

    ASSERT_EQ(dev, mock_write_count, 2U);
    ASSERT_EQ(dev, mock_read_count,  2U);
}

/* =========================================================================
 * Group C — Multi-Sector Operations
 * ========================================================================= */

/** C1: Write 3 sectors, read them back through block_dev. */
static void test_block_dev_multi_sector(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = MOCK_SECTOR_SIZE, .total_sectors = MOCK_SECTORS };
    uint8_t id;
    block_dev_register(&mock, &id);

    uint8_t write_buf[3 * MOCK_SECTOR_SIZE];
    uint8_t read_buf[3 * MOCK_SECTOR_SIZE];

    for (int i = 0; i < 3 * MOCK_SECTOR_SIZE; i++)
        write_buf[i] = (uint8_t)(i & 0xFF);

    block_status_t st = block_dev_write(id, 2, 3, write_buf);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_OK);

    memset(read_buf, 0, sizeof(read_buf));
    st = block_dev_read(id, 2, 3, read_buf);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_OK);

    for (int i = 0; i < 3 * MOCK_SECTOR_SIZE; i++) {
        if (read_buf[i] != write_buf[i]) {
            serial_write_string(dev, "[BDEV TEST]   FAIL: multi byte ");
            t_hex(dev, (uint32_t)i);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
}

/** C2: Read sector at last valid index (MOCK_SECTORS - 1). */
static void test_block_dev_read_last_sector(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = MOCK_SECTOR_SIZE, .total_sectors = MOCK_SECTORS };
    uint8_t id;
    block_dev_register(&mock, &id);

    /* Write a known pattern to the last sector. */
    uint8_t pattern[MOCK_SECTOR_SIZE];
    memset(pattern, 0xBB, sizeof(pattern));
    block_dev_write(id, MOCK_SECTORS - 1, 1, pattern);

    uint8_t read_buf[MOCK_SECTOR_SIZE];
    block_status_t st = block_dev_read(id, MOCK_SECTORS - 1, 1, read_buf);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_OK);

    for (int i = 0; i < MOCK_SECTOR_SIZE; i++) {
        if (read_buf[i] != 0xBB) {
            test_fail_flag = 1;
            return;
        }
    }
}

/* =========================================================================
 * Group D — Edge Cases and Error Handling
 * ========================================================================= */

/** D1: Read from an unregistered device returns error. */
static void test_block_dev_read_unregistered(serial_dev_t *dev) {
    unregister_all();
    uint8_t buf[512];
    block_status_t st = block_dev_read(0, 0, 1, buf);
    t_log(dev, "read_unreg", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NOT_FOUND);
}

/** D2: Write to an unregistered device returns error. */
static void test_block_dev_write_unregistered(serial_dev_t *dev) {
    unregister_all();
    uint8_t buf[512];
    block_status_t st = block_dev_write(0, 0, 1, buf);
    t_log(dev, "write_unreg", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NOT_FOUND);
}

/** D3: Read with NULL buffer returns error. */
static void test_block_dev_read_null(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_dev_register(&mock, &id);

    block_status_t st = block_dev_read(id, 0, 1, NULL);
    t_log(dev, "read_null", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NULL_BUF);
}

/** D4: Write with NULL buffer returns error. */
static void test_block_dev_write_null(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_dev_register(&mock, &id);

    block_status_t st = block_dev_write(id, 0, 1, NULL);
    t_log(dev, "write_null", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NULL_BUF);
}

/** D5: Read with count=0 returns error. */
static void test_block_dev_read_zero_count(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_dev_register(&mock, &id);

    uint8_t buf[512];
    block_status_t st = block_dev_read(id, 0, 0, buf);
    t_log(dev, "read_zero", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_INVALID);
}

/** D6: Write with count=0 returns error. */
static void test_block_dev_write_zero_count(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_dev_register(&mock, &id);

    uint8_t buf[512];
    block_status_t st = block_dev_write(id, 0, 0, buf);
    t_log(dev, "write_zero", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_INVALID);
}

/** D7: Read with out-of-range device ID returns error. */
static void test_block_dev_read_bad_id(serial_dev_t *dev) {
    uint8_t buf[512];
    block_status_t st = block_dev_read(BLOCK_DEV_MAX + 1, 0, 1, buf);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NOT_FOUND);
}

/** D8: Write with out-of-range device ID returns error. */
static void test_block_dev_write_bad_id(serial_dev_t *dev) {
    uint8_t buf[512] = {0};
    block_status_t st = block_dev_write(BLOCK_DEV_MAX + 1, 0, 1, buf);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_ERR_NOT_FOUND);
}

/** D9: Mock device returns error — block_dev_read propagates it. */
static void test_block_dev_read_propagates_error(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_dev_register(&mock, &id);

    /* Simulate a hardware error. */
    mock_simulated_error = -99;

    uint8_t buf[512];
    block_status_t st = block_dev_read(id, 0, 1, buf);
    t_log(dev, "read_propagates", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != BLOCK_OK);

    mock_simulated_error = 0; /* restore */
}

/** D10: Mock device returns error — block_dev_write propagates it. */
static void test_block_dev_write_propagates_error(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_dev_register(&mock, &id);

    mock_simulated_error = -99;

    uint8_t buf[512];
    block_status_t st = block_dev_write(id, 0, 1, buf);
    t_log(dev, "write_propagates", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != BLOCK_OK);

    mock_simulated_error = 0;
}

/** D11: Multi-sector write where middle sector's mock returns error. */
static void test_block_dev_multi_sector_partial_error(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = 512, .total_sectors = 16 };
    uint8_t id;
    block_dev_register(&mock, &id);

    /* Pre-fill first two sectors with known data. */
    uint8_t pre[512];
    memset(pre, 0x11, sizeof(pre));
    block_dev_write(id, 0, 1, pre);
    block_dev_write(id, 1, 1, pre);
    block_dev_write(id, 2, 1, pre);

    /* Now attempt a 3-sector read with simulated error.
     * The first sector should succeed, the second triggers error. */
    uint8_t read_buf[3 * 512];
    memset(read_buf, 0, sizeof(read_buf));

    /* Write first two sectors successfully. */
    block_status_t st = block_dev_write(id, 0, 2, pre);
    ASSERT_EQ(dev, (int)st, (int)BLOCK_OK);

    /* Now simulate error and try to read 3 sectors. */
    mock_simulated_error = -99;
    st = block_dev_read(id, 0, 3, read_buf);
    ASSERT_TRUE(dev, st != BLOCK_OK);

    mock_simulated_error = 0;
}

/** D12: sector_size and total_sectors queries. */
static void test_block_dev_geometry(serial_dev_t *dev) {
    unregister_all();
    mock_reset();

    block_device_t mock = { .read_sector = mock_read, .write_sector = mock_write,
                            .sector_size = MOCK_SECTOR_SIZE, .total_sectors = MOCK_SECTORS };
    uint8_t id;
    block_dev_register(&mock, &id);

    uint32_t ss = block_dev_sector_size(id);
    uint64_t ts = block_dev_total_sectors(id);

    t_log(dev, "geometry: sector_size", ss);
    t_log(dev, "geometry: total_sectors", (uint32_t)ts);

    ASSERT_EQ(dev, ss, (uint32_t)MOCK_SECTOR_SIZE);
    ASSERT_EQ(dev, ts, (uint64_t)MOCK_SECTORS);
}

/** D13: Geometry query for unregistered device returns 0. */
static void test_block_dev_geometry_absent(serial_dev_t *dev) {
    uint32_t ss = block_dev_sector_size(5);
    uint64_t ts = block_dev_total_sectors(5);
    ASSERT_EQ(dev, ss, 0U);
    ASSERT_EQ(dev, ts, (uint64_t)0);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_block_dev(void) {
    /* Group A — Registration */
    test_register("bdev_register_first",     test_block_dev_register_first);
    test_register("bdev_register_multi",     test_block_dev_register_multiple);
    test_register("bdev_register_null",      test_block_dev_register_null);
    test_register("bdev_register_null_id",   test_block_dev_register_null_id);
    test_register("bdev_register_null_fns",  test_block_dev_register_null_fns);
    test_register("bdev_register_null_read", test_block_dev_register_null_read_fn);
    test_register("bdev_get_present",        test_block_dev_get_present);
    test_register("bdev_get_absent",         test_block_dev_get_absent);
    test_register("bdev_get_out_of_range",   test_block_dev_get_out_of_range);
    test_register("bdev_unregister_works",   test_block_dev_unregister_works);
    test_register("bdev_unregister_notfnd",  test_block_dev_unregister_not_found);

    /* Group B — I/O */
    test_register("bdev_write_read_single",  test_block_dev_write_read_single);
    test_register("bdev_counter_increment",  test_block_dev_counter_increment);

    /* Group C — Multi-sector */
    test_register("bdev_multi_sector",       test_block_dev_multi_sector);
    test_register("bdev_read_last_sector",   test_block_dev_read_last_sector);

    /* Group D — Edge cases */
    test_register("bdev_read_unreg",         test_block_dev_read_unregistered);
    test_register("bdev_write_unreg",        test_block_dev_write_unregistered);
    test_register("bdev_read_null",          test_block_dev_read_null);
    test_register("bdev_write_null",         test_block_dev_write_null);
    test_register("bdev_read_zero",          test_block_dev_read_zero_count);
    test_register("bdev_write_zero",         test_block_dev_write_zero_count);
    test_register("bdev_read_bad_id",        test_block_dev_read_bad_id);
    test_register("bdev_write_bad_id",       test_block_dev_write_bad_id);
    test_register("bdev_read_prop_err",      test_block_dev_read_propagates_error);
    test_register("bdev_write_prop_err",     test_block_dev_write_propagates_error);
    test_register("bdev_multi_partial_err",  test_block_dev_multi_sector_partial_error);
    test_register("bdev_geometry",           test_block_dev_geometry);
    test_register("bdev_geometry_absent",    test_block_dev_geometry_absent);
}
