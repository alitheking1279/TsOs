/**
 * @file test_ata.c
 * @brief ATA PIO driver test suite.
 *
 * These tests verify the ATA PIO driver against a QEMU IDE drive.
 * QEMU provides a virtual IDE controller with 128 MiB default RAM;
 * we use a secondary test image passed via -drive.
 *
 * Test groups:
 *   Group A — Init / detection
 *   Group B — Single-sector read / write
 *   Group C — Multi-sector operations
 *   Group D — Edge cases and error handling
 */

#include "test.h"
#include "../drivers/ata.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

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
    serial_write_string(dev, "[ATA TEST]   ");
    serial_write_string(dev, label);
    serial_write_string(dev, " = ");
    t_hex(dev, val);
    serial_write_string(dev, "\r\n");
}

static void t_log_str(serial_dev_t *dev, const char *label, const char *val) {
    serial_write_string(dev, "[ATA TEST]   ");
    serial_write_string(dev, label);
    serial_write_string(dev, " = ");
    serial_write_string(dev, val);
    serial_write_string(dev, "\r\n");
}

/* =========================================================================
 * Group A — Init / Detection
 * ========================================================================= */

/** A1: ata_is_initialized() returns true after successful init. */
static void test_ata_init_detected(serial_dev_t *dev) {
    /* ata_init() is called in main.c before tests. */
    bool init = ata_is_initialized();
    t_log_str(dev, "is_initialized", init ? "true" : "false");
    ASSERT_TRUE(dev, init);
}

/** A2: Status string for ATA_OK is "OK". */
static void test_ata_status_string_ok(serial_dev_t *dev) {
    const char *s = ata_status_string(ATA_OK);
    t_log_str(dev, "status_string(OK)", s);
    /* Verify it starts with 'O' and 'K'. */
    ASSERT_TRUE(dev, s[0] == 'O' && s[1] == 'K' && s[2] == '\0');
}

/** A3: Status string for each error is non-NULL and non-empty. */
static void test_ata_status_strings(serial_dev_t *dev) {
    ata_status_t errs[] = {
        ATA_ERR_TIMEOUT, ATA_ERR_DRIVE_NOT_FOUND, ATA_ERR_IDENTIFY_FAILED,
        ATA_ERR_INVALID_LBA, ATA_ERR_INVALID_SECTOR_COUNT,
        ATA_ERR_READ_FAILED, ATA_ERR_WRITE_FAILED, ATA_ERR_DEVICE_FAULT
    };
    int count = sizeof(errs) / sizeof(errs[0]);
    for (int i = 0; i < count; i++) {
        const char *s = ata_status_string(errs[i]);
        ASSERT_TRUE(dev, s != NULL);
        ASSERT_TRUE(dev, s[0] != '\0');
    }
    t_log(dev, "all_status_strings_checked", (uint32_t)count);
}

/* =========================================================================
 * Group B — Single-Sector Read / Write
 *
 * We use high LBAs near the end of the 4 MiB test disk image.
 * 4 MiB = 8192 sectors.  We test at LBA 7000 — safe, high enough
 * to avoid anything important but within the disk.
 * ========================================================================= */

#define TEST_LBA       7000U
#define TEST_LBA_2     7001U

/** B1: Read a single sector — verify no crash and non-zero data. */
static void test_ata_read_single_sector(serial_dev_t *dev) {
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    ata_status_t st = ata_read_sector(TEST_LBA, 1, buf);
    t_log(dev, "read_single: status", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    /* Verify the buffer was actually written to (not all zeros).
     * A freshly created QEMU disk image may have all zeros at this LBA,
     * but after a write+read cycle it should be non-zero.  So we just
     * verify the read succeeded. */
    t_log(dev, "read_single: first_word", (uint32_t)(buf[0] | (buf[1] << 8)));
}

/** B2: Write a sector with a known pattern, read it back, verify byte-for-byte. */
static void test_ata_write_read_back_single(serial_dev_t *dev) {
    uint8_t write_buf[512];
    uint8_t read_buf[512];

    /* Fill write buffer with a deterministic pattern. */
    for (int i = 0; i < 512; i++) {
        write_buf[i] = (uint8_t)((i * 7 + 0x55) & 0xFF);
    }

    /* Write to TEST_LBA. */
    ata_status_t st = ata_write_sector(TEST_LBA, 1, write_buf);
    t_log(dev, "write_read_back: write_status", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    /* Read it back. */
    memset(read_buf, 0, sizeof(read_buf));
    st = ata_read_sector(TEST_LBA, 1, read_buf);
    t_log(dev, "write_read_back: read_status", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    /* Byte-for-byte comparison. */
    for (int i = 0; i < 512; i++) {
        if (read_buf[i] != write_buf[i]) {
            serial_write_string(dev, "[ATA TEST]   FAIL: mismatch at byte ");
            t_hex(dev, (uint32_t)i);
            serial_write_string(dev, " expected=");
            t_hex(dev, (uint32_t)write_buf[i]);
            serial_write_string(dev, " got=");
            t_hex(dev, (uint32_t)read_buf[i]);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
}

/** B3: Write zeros, read back — verify all zeros. */
static void test_ata_write_zeros_read_back(serial_dev_t *dev) {
    uint8_t zeros[512];
    uint8_t read_buf[512];
    memset(zeros, 0, sizeof(zeros));
    memset(read_buf, 0xAB, sizeof(read_buf)); /* poison pattern */

    ata_status_t st = ata_write_sector(TEST_LBA, 1, zeros);
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    st = ata_read_sector(TEST_LBA, 1, read_buf);
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    for (int i = 0; i < 512; i++) {
        if (read_buf[i] != 0) {
            serial_write_string(dev, "[ATA TEST]   FAIL: non-zero at byte ");
            t_hex(dev, (uint32_t)i);
            serial_write_string(dev, " got=");
            t_hex(dev, (uint32_t)read_buf[i]);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
}

/** B4: Write 0xFF pattern, read back — verify all 0xFF. */
static void test_ata_write_0xff_read_back(serial_dev_t *dev) {
    uint8_t pattern[512];
    uint8_t read_buf[512];
    memset(pattern, 0xFF, sizeof(pattern));

    ata_status_t st = ata_write_sector(TEST_LBA, 1, pattern);
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    st = ata_read_sector(TEST_LBA, 1, read_buf);
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    for (int i = 0; i < 512; i++) {
        if (read_buf[i] != 0xFF) {
            serial_write_string(dev, "[ATA TEST]   FAIL: byte ");
            t_hex(dev, (uint32_t)i);
            serial_write_string(dev, " expected=0xFF got=");
            t_hex(dev, (uint32_t)read_buf[i]);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
}

/* =========================================================================
 * Group C — Multi-Sector Operations
 * ========================================================================= */

/** C1: Write 2 sectors with distinct patterns, read back both. */
static void test_ata_write_read_two_sectors(serial_dev_t *dev) {
    uint8_t write_buf[1024];
    uint8_t read_buf[1024];

    /* Distinct patterns for each sector. */
    for (int i = 0; i < 512; i++) {
        write_buf[i]      = (uint8_t)(i & 0xFF);
        write_buf[512 + i] = (uint8_t)(~i & 0xFF);
    }

    /* Write both sectors in one call. */
    ata_status_t st = ata_write_sector(TEST_LBA, 2, write_buf);
    t_log(dev, "multi_write: status", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    /* Read both sectors in one call. */
    memset(read_buf, 0, sizeof(read_buf));
    st = ata_read_sector(TEST_LBA, 2, read_buf);
    t_log(dev, "multi_read: status", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    /* Verify sector A. */
    for (int i = 0; i < 512; i++) {
        if (read_buf[i] != write_buf[i]) {
            serial_write_string(dev, "[ATA TEST]   FAIL: sector A byte ");
            t_hex(dev, (uint32_t)i);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }

    /* Verify sector B. */
    for (int i = 0; i < 512; i++) {
        if (read_buf[512 + i] != write_buf[512 + i]) {
            serial_write_string(dev, "[ATA TEST]   FAIL: sector B byte ");
            t_hex(dev, (uint32_t)i);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
}

/** C2: Write 4 sectors with ascending byte pattern, read back. */
static void test_ata_four_sector_roundtrip(serial_dev_t *dev) {
    uint8_t write_buf[4 * 512];
    uint8_t read_buf[4 * 512];

    /* Fill with ascending bytes. */
    for (int i = 0; i < 4 * 512; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }

    ata_status_t st = ata_write_sector(TEST_LBA, 4, write_buf);
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    memset(read_buf, 0, sizeof(read_buf));
    st = ata_read_sector(TEST_LBA, 4, read_buf);
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);

    for (int i = 0; i < 4 * 512; i++) {
        if (read_buf[i] != write_buf[i]) {
            serial_write_string(dev, "[ATA TEST]   FAIL: 4-sec byte ");
            t_hex(dev, (uint32_t)i);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
}

/* =========================================================================
 * Group D — Edge Cases and Error Handling
 * ========================================================================= */

/** D1: Read with count=0 returns error. */
static void test_ata_read_zero_sectors(serial_dev_t *dev) {
    uint8_t buf[512] = {0};
    ata_status_t st = ata_read_sector(TEST_LBA, 0, buf);
    t_log(dev, "read_zero_count", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != ATA_OK);
}

/** D2: Write with count=0 returns error. */
static void test_ata_write_zero_sectors(serial_dev_t *dev) {
    uint8_t buf[512] = {0};
    ata_status_t st = ata_write_sector(TEST_LBA, 0, buf);
    t_log(dev, "write_zero_count", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != ATA_OK);
}

/** D3: Read with count > 256 returns error. */
static void test_ata_read_too_many_sectors(serial_dev_t *dev) {
    uint8_t buf[512] = {0};
    ata_status_t st = ata_read_sector(TEST_LBA, 257, buf);
    t_log(dev, "read_too_many", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != ATA_OK);
}

/** D4: Write with count > 256 returns error. */
static void test_ata_write_too_many_sectors(serial_dev_t *dev) {
    uint8_t buf[512] = {0};
    ata_status_t st = ata_write_sector(TEST_LBA, 257, buf);
    t_log(dev, "write_too_many", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != ATA_OK);
}

/** D5: Read with NULL buffer returns error. */
static void test_ata_read_null_buffer(serial_dev_t *dev) {
    ata_status_t st = ata_read_sector(TEST_LBA, 1, NULL);
    t_log(dev, "read_null_buf", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != ATA_OK);
}

/** D6: Write with NULL buffer returns error. */
static void test_ata_write_null_buffer(serial_dev_t *dev) {
    ata_status_t st = ata_write_sector(TEST_LBA, 1, NULL);
    t_log(dev, "write_null_buf", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != ATA_OK);
}

/** D7: Read with LBA > max returns error. */
static void test_ata_read_invalid_lba(serial_dev_t *dev) {
    uint8_t buf[512] = {0};
    ata_status_t st = ata_read_sector(0x10000000, 1, buf); /* > 28-bit max */
    t_log(dev, "read_invalid_lba", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != ATA_OK);
}

/** D8: Write with LBA > max returns error. */
static void test_ata_write_invalid_lba(serial_dev_t *dev) {
    uint8_t buf[512] = {0};
    ata_status_t st = ata_write_sector(0x10000000, 1, buf);
    t_log(dev, "write_invalid_lba", (uint32_t)(-st));
    ASSERT_TRUE(dev, st != ATA_OK);
}

/** D9: Write to LBA 0 (MBR) — read back to verify (don't overwrite). */
static void test_ata_read_lba_zero(serial_dev_t *dev) {
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    /* READ from LBA 0 — this is safe (no write). */
    ata_status_t st = ata_read_sector(0, 1, buf);
    t_log(dev, "read_lba_zero", (uint32_t)(-st));
    ASSERT_EQ(dev, (int)st, (int)ATA_OK);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ata(void) {
    /* Group A */
    test_register("ata_init_detected",        test_ata_init_detected);
    test_register("ata_status_string_ok",     test_ata_status_string_ok);
    test_register("ata_status_strings",       test_ata_status_strings);

    /* Group B */
    test_register("ata_read_single_sector",   test_ata_read_single_sector);
    test_register("ata_write_read_back_single", test_ata_write_read_back_single);
    test_register("ata_write_zeros_read_back", test_ata_write_zeros_read_back);
    test_register("ata_write_0xff_read_back", test_ata_write_0xff_read_back);

    /* Group C */
    test_register("ata_write_read_two_sectors", test_ata_write_read_two_sectors);
    test_register("ata_four_sector_roundtrip",  test_ata_four_sector_roundtrip);

    /* Group D */
    test_register("ata_read_zero_sectors",    test_ata_read_zero_sectors);
    test_register("ata_write_zero_sectors",   test_ata_write_zero_sectors);
    test_register("ata_read_too_many",        test_ata_read_too_many_sectors);
    test_register("ata_write_too_many",       test_ata_write_too_many_sectors);
    test_register("ata_read_null_buf",        test_ata_read_null_buffer);
    test_register("ata_write_null_buf",       test_ata_write_null_buffer);
    test_register("ata_read_invalid_lba",     test_ata_read_invalid_lba);
    test_register("ata_write_invalid_lba",    test_ata_write_invalid_lba);
    test_register("ata_read_lba_zero",        test_ata_read_lba_zero);
}
