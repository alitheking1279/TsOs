/**
 * @file test_bcache.c
 * @brief Buffer cache test suite.
 *
 * Tests the bcache API using a mock (RAM-backed) block device.
 *
 * Test groups:
 *   Group A — Init / Lifecycle
 *   Group B — Read-through (get from disk)
 *   Group C — Dirty / Write-back
 *   Group D — Eviction / LRU / Invalidation
 *   Group E — Edge cases and statistics
 */

#include "test.h"
#include "../fs/bcache.h"
#include "../fs/block_dev.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Mock Device — backed by a RAM buffer
 * ========================================================================= */

#define MOCK_SECTORS     256
#define MOCK_SECTOR_SIZE 512

static uint8_t mock_storage[MOCK_SECTORS * MOCK_SECTOR_SIZE];
static uint32_t mock_read_count;
static uint32_t mock_write_count;

static block_status_t mock_read(uint8_t dev_id, uint32_t lba, void *buf) {
    (void)dev_id;
    if (lba >= MOCK_SECTORS) return BLOCK_ERR_INVALID;
    memcpy(buf, &mock_storage[lba * MOCK_SECTOR_SIZE], MOCK_SECTOR_SIZE);
    mock_read_count++;
    return BLOCK_OK;
}

static block_status_t mock_write(uint8_t dev_id, uint32_t lba, const void *buf) {
    (void)dev_id;
    if (lba >= MOCK_SECTORS) return BLOCK_ERR_INVALID;
    memcpy(&mock_storage[lba * MOCK_SECTOR_SIZE], buf, MOCK_SECTOR_SIZE);
    mock_write_count++;
    return BLOCK_OK;
}

static void mock_reset(void) {
    memset(mock_storage, 0, sizeof(mock_storage));
    mock_read_count = 0;
    mock_write_count = 0;
}

/**
 * @brief Fill a sector in mock storage with a unique pattern.
 */
static void mock_fill_sector(uint32_t lba, uint8_t byte) {
    if (lba >= MOCK_SECTORS) return;
    memset(&mock_storage[lba * MOCK_SECTOR_SIZE], byte, MOCK_SECTOR_SIZE);
}

/**
 * @brief Register the mock device and return its ID.
 */
static uint8_t mock_register(void) {
    block_device_t dev = {
        .read_sector  = mock_read,
        .write_sector = mock_write,
        .sector_size  = MOCK_SECTOR_SIZE,
        .total_sectors = MOCK_SECTORS,
    };
    uint8_t id = 0xFF;
    block_dev_register(&dev, &id);
    return id;
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void t_log(serial_dev_t *dev, const char *label, uint32_t val) {
    serial_write_string(dev, "[BC TEST]   ");
    serial_write_string(dev, label);
    serial_write_string(dev, " = ");
    static const char h[] = "0123456789ABCDEF";
    serial_write_string(dev, "0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_write_char(dev, h[(val >> i) & 0xF]);
    serial_write_string(dev, "\r\n");
}

/* =========================================================================
 * Group A — Init / Lifecycle
 * ========================================================================= */

/** A1: Init sets total_buffers to BCACHE_NBUFS. */
static void test_bcache_init_total(serial_dev_t *dev) {
    mock_reset();
    block_dev_unregister(0);
    bcache_init(dev);

    bcache_stats_t st;
    bcache_get_stats(&st);
    t_log(dev, "init_total", st.total_buffers);
    ASSERT_EQ(dev, st.total_buffers, (uint32_t)BCACHE_NBUFS);
}

/** A2: Init leaves all buffers free. */
static void test_bcache_init_free(serial_dev_t *dev) {
    bcache_init(dev);

    bcache_stats_t st;
    bcache_get_stats(&st);
    t_log(dev, "init_free", st.free_buffers);
    ASSERT_EQ(dev, st.free_buffers, (uint32_t)BCACHE_NBUFS);
}

/** A3: Init zeros valid and dirty counts. */
static void test_bcache_init_zero_counts(serial_dev_t *dev) {
    bcache_init(dev);

    bcache_stats_t st;
    bcache_get_stats(&st);
    ASSERT_EQ(dev, st.valid_buffers, 0U);
    ASSERT_EQ(dev, st.dirty_buffers, 0U);
}

/** A4: Reset stats clears all counters. */
static void test_bcache_reset_stats(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    /* Do a get to bump counters. */
    buf_t *b = bcache_get(id, 0);
    ASSERT_NOT_NULL(dev, b);
    bcache_put(b);

    bcache_reset_stats();
    bcache_stats_t st;
    bcache_get_stats(&st);
    ASSERT_EQ(dev, st.hash_hits, 0U);
    ASSERT_EQ(dev, st.hash_misses, 0U);
    ASSERT_EQ(dev, st.evictions, 0U);
    ASSERT_EQ(dev, st.flushes, 0U);
}

/** A5: bcache_init can be called multiple times (re-init). */
static void test_bcache_init_reinit(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    /* Pollute the cache. */
    buf_t *b = bcache_get(id, 0);
    bcache_dirty(b);
    bcache_put(b);

    /* Re-init should clear everything. */
    bcache_init(dev);
    bcache_stats_t st;
    bcache_get_stats(&st);
    ASSERT_EQ(dev, st.valid_buffers, 0U);
    ASSERT_EQ(dev, st.dirty_buffers, 0U);
}

/* =========================================================================
 * Group B — Read-through
 * ========================================================================= */

/** B1: bcache_get reads sector data from mock device. */
static void test_bcache_get_read_data(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    /* Fill sector 5 with 0xAA. */
    mock_fill_sector(5, 0xAA);

    buf_t *b = bcache_get(id, 5);
    ASSERT_NOT_NULL(dev, b);

    /* Verify data. */
    for (int i = 0; i < MOCK_SECTOR_SIZE; i++) {
        if (b->data[i] != 0xAA) {
            test_fail_flag = 1;
            return;
        }
    }
    bcache_put(b);
}

/** B2: Same (dev, blk) returns same buffer (cache hit). */
static void test_bcache_get_same_buffer(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b1 = bcache_get(id, 3);
    buf_t *b2 = bcache_get(id, 3);

    ASSERT_NOT_NULL(dev, b1);
    ASSERT_NOT_NULL(dev, b2);
    ASSERT_TRUE(dev, b1 == b2);

    bcache_put(b1);
    bcache_put(b2);
}

/** B3: Cache hit increments ref_count. */
static void test_bcache_get_refcount(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b1 = bcache_get(id, 7);
    ASSERT_NOT_NULL(dev, b1);
    ASSERT_EQ(dev, b1->ref_count, 1U);

    buf_t *b2 = bcache_get(id, 7);
    ASSERT_NOT_NULL(dev, b2);
    ASSERT_EQ(dev, b1->ref_count, 2U);

    bcache_put(b1);
    ASSERT_EQ(dev, b1->ref_count, 1U);

    bcache_put(b2);
    ASSERT_EQ(dev, b1->ref_count, 0U);
}

/** B4: bcache_put decrements ref_count. */
static void test_bcache_put_decrements(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b = bcache_get(id, 0);
    ASSERT_NOT_NULL(dev, b);
    ASSERT_EQ(dev, b->ref_count, 1U);

    bcache_put(b);
    ASSERT_EQ(dev, b->ref_count, 0U);
}

/** B5: Different blocks get different buffers. */
static void test_bcache_get_different_blocks(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    mock_fill_sector(10, 0x11);
    mock_fill_sector(20, 0x22);

    buf_t *b1 = bcache_get(id, 10);
    buf_t *b2 = bcache_get(id, 20);

    ASSERT_NOT_NULL(dev, b1);
    ASSERT_NOT_NULL(dev, b2);
    ASSERT_TRUE(dev, b1 != b2);

    /* Verify different data. */
    ASSERT_EQ(dev, b1->data[0], 0x11);
    ASSERT_EQ(dev, b2->data[0], 0x22);

    bcache_put(b1);
    bcache_put(b2);
}

/** B6: Read mock_read_count increments on cache miss. */
static void test_bcache_get_disk_io(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    uint32_t before = mock_read_count;
    buf_t *b = bcache_get(id, 2);
    ASSERT_NOT_NULL(dev, b);
    t_log(dev, "disk_io_before", before);
    t_log(dev, "disk_io_after", mock_read_count);
    ASSERT_EQ(dev, mock_read_count, before + 1);

    /* Second get should not read from disk (cache hit). */
    buf_t *b2 = bcache_get(id, 2);
    ASSERT_NOT_NULL(dev, b2);
    ASSERT_EQ(dev, mock_read_count, before + 1);

    bcache_put(b);
    bcache_put(b2);
}

/* =========================================================================
 * Group C — Dirty / Write-back
 * ========================================================================= */

/** C1: bcache_dirty sets DIRTY flag. */
static void test_bcache_dirty_flag(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b = bcache_get(id, 0);
    ASSERT_NOT_NULL(dev, b);
    ASSERT_TRUE(dev, !(b->flags & BCACHE_FLAG_DIRTY));

    bcache_dirty(b);
    ASSERT_TRUE(dev, b->flags & BCACHE_FLAG_DIRTY);

    bcache_put(b);
}

/** C2: bcache_flush_dev writes dirty buffer to mock storage. */
static void test_bcache_flush_writes(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b = bcache_get(id, 1);
    ASSERT_NOT_NULL(dev, b);

    /* Modify the buffer data. */
    memset(b->data, 0xBB, MOCK_SECTOR_SIZE);
    bcache_dirty(b);

    /* Flush should write to mock storage. */
    uint32_t writes_before = mock_write_count;
    bcache_status_t st = bcache_flush_dev(id);
    ASSERT_EQ(dev, (int)st, (int)BCACHE_OK);
    t_log(dev, "flush_writes", mock_write_count);
    ASSERT_EQ(dev, mock_write_count, writes_before + 1);

    /* Verify mock storage has the data. */
    for (int i = 0; i < MOCK_SECTOR_SIZE; i++) {
        if (mock_storage[1 * MOCK_SECTOR_SIZE + i] != 0xBB) {
            test_fail_flag = 1;
            return;
        }
    }

    bcache_put(b);
}

/** C3: bcache_flush_dev clears DIRTY flag after write. */
static void test_bcache_flush_clears_dirty(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b = bcache_get(id, 2);
    ASSERT_NOT_NULL(dev, b);
    memset(b->data, 0xCC, MOCK_SECTOR_SIZE);
    bcache_dirty(b);
    ASSERT_TRUE(dev, b->flags & BCACHE_FLAG_DIRTY);

    bcache_flush_dev(id);
    ASSERT_TRUE(dev, !(b->flags & BCACHE_FLAG_DIRTY));

    bcache_put(b);
}

/** C4: bcache_flush_dev skips clean buffers. */
static void test_bcache_flush_skips_clean(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b = bcache_get(id, 0);
    ASSERT_NOT_NULL(dev, b);
    /* Don't dirty it. */

    uint32_t writes_before = mock_write_count;
    bcache_flush_dev(id);
    ASSERT_EQ(dev, mock_write_count, writes_before);

    bcache_put(b);
}

/** C5: bcache_flush_all writes all dirty buffers. */
static void test_bcache_flush_all(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b1 = bcache_get(id, 0);
    buf_t *b2 = bcache_get(id, 1);
    buf_t *b3 = bcache_get(id, 2);

    memset(b1->data, 0x11, MOCK_SECTOR_SIZE);
    memset(b2->data, 0x22, MOCK_SECTOR_SIZE);
    /* b3 stays clean. */

    bcache_dirty(b1);
    bcache_dirty(b2);

    uint32_t writes_before = mock_write_count;
    bcache_flush_all();
    t_log(dev, "flush_all_writes", mock_write_count);
    ASSERT_EQ(dev, mock_write_count, writes_before + 2);

    bcache_put(b1);
    bcache_put(b2);
    bcache_put(b3);
}

/* =========================================================================
 * Group D — Eviction / LRU / Invalidation
 * ========================================================================= */

/** D1: Eviction of dirty buffer writes back to disk. */
static void test_bcache_evict_dirty_writeback(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    /* Fill the entire cache (128 buffers) with different blocks. */
    buf_t *bufs[BCACHE_NBUFS];
    for (int i = 0; i < BCACHE_NBUFS; i++) {
        mock_fill_sector((uint32_t)i, (uint8_t)(i & 0xFF));
        bufs[i] = bcache_get(id, (uint32_t)i);
        ASSERT_NOT_NULL(dev, bufs[i]);
        memset(bufs[i]->data, (uint8_t)(0x80 + i), MOCK_SECTOR_SIZE);
        bcache_dirty(bufs[i]);
    }

    /* All 128 slots are pinned (ref=1).  Can't evict yet. */
    /* Release all refs. */
    for (int i = 0; i < BCACHE_NBUFS; i++) {
        bcache_put(bufs[i]);
    }

    /* Now get a 129th block — must evict one dirty buffer. */
    uint32_t writes_before = mock_write_count;
    mock_fill_sector(BCACHE_NBUFS, 0xDD);
    buf_t *extra = bcache_get(id, (uint32_t)BCACHE_NBUFS);
    ASSERT_NOT_NULL(dev, extra);
    t_log(dev, "evict_wb", mock_write_count);
    ASSERT_TRUE(dev, mock_write_count > writes_before);

    bcache_put(extra);
}

/** D2: Eviction of clean buffer does NOT write to disk. */
static void test_bcache_evict_clean_no_write(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    /* Fill cache with clean buffers. */
    buf_t *bufs[BCACHE_NBUFS];
    for (int i = 0; i < BCACHE_NBUFS; i++) {
        mock_fill_sector((uint32_t)i, (uint8_t)i);
        bufs[i] = bcache_get(id, (uint32_t)i);
        ASSERT_NOT_NULL(dev, bufs[i]);
        /* Don't dirty. */
    }

    for (int i = 0; i < BCACHE_NBUFS; i++) bcache_put(bufs[i]);

    uint32_t writes_before = mock_write_count;
    mock_fill_sector(BCACHE_NBUFS, 0xEE);
    buf_t *extra = bcache_get(id, (uint32_t)BCACHE_NBUFS);
    ASSERT_NOT_NULL(dev, extra);
    t_log(dev, "evict_clean", mock_write_count);
    ASSERT_EQ(dev, mock_write_count, writes_before);

    bcache_put(extra);
}

/** D3: Pinned buffers (ref > 0) are not evicted. */
static void test_bcache_no_evict_pinned(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    /* Pin a buffer. */
    buf_t *pinned = bcache_get(id, 0);
    ASSERT_NOT_NULL(dev, pinned);
    uint32_t pin_addr = (uint32_t)(uintptr_t)pinned;

    /* Fill rest of cache. */
    for (int i = 1; i <= BCACHE_NBUFS; i++) {
        mock_fill_sector((uint32_t)i, (uint8_t)i);
        buf_t *b = bcache_get(id, (uint32_t)i);
        ASSERT_NOT_NULL(dev, b);
        bcache_put(b);
    }

    /* Try to get one more — the pinned buffer must survive. */
    mock_fill_sector(BCACHE_NBUFS + 1, 0xFF);
    buf_t *extra = bcache_get(id, (uint32_t)(BCACHE_NBUFS + 1));
    ASSERT_NOT_NULL(dev, extra);

    /* The pinned buffer should still be the same pointer. */
    ASSERT_TRUE(dev, (uint32_t)(uintptr_t)pinned == pin_addr);

    bcache_put(pinned);
    bcache_put(extra);
}

/** D4: bcache_invalidate_dev removes buffers for that device. */
static void test_bcache_invalidate_dev(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b = bcache_get(id, 5);
    ASSERT_NOT_NULL(dev, b);
    bcache_put(b);

    bcache_invalidate_dev(id);

    /* Next get for block 5 should be a cache miss (new read). */
    uint32_t reads_before = mock_read_count;
    buf_t *b2 = bcache_get(id, 5);
    ASSERT_NOT_NULL(dev, b2);
    t_log(dev, "inv_dev_reads", mock_read_count);
    ASSERT_EQ(dev, mock_read_count, reads_before + 1);

    bcache_put(b2);
}

/** D5: bcache_invalidate_all clears everything. */
static void test_bcache_invalidate_all(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b = bcache_get(id, 10);
    ASSERT_NOT_NULL(dev, b);
    bcache_put(b);

    bcache_invalidate_all();

    bcache_stats_t st;
    bcache_get_stats(&st);
    ASSERT_EQ(dev, st.valid_buffers, 0U);
    ASSERT_EQ(dev, st.dirty_buffers, 0U);
}

/* =========================================================================
 * Group E — Edge Cases and Statistics
 * ========================================================================= */

/** E1: bcache_put(NULL) is a no-op. */
static void test_bcache_put_null(serial_dev_t *dev) {
    bcache_init(dev);
    /* Should not crash. */
    bcache_put(NULL);
    ASSERT_TRUE(dev, 1);  /* If we reach here, no crash. */
}

/** E2: bcache_dirty(NULL) is a no-op. */
static void test_bcache_dirty_null(serial_dev_t *dev) {
    bcache_init(dev);
    bcache_dirty(NULL);
    ASSERT_TRUE(dev, 1);
}

/** E3: bcache_get_stats returns consistent counts. */
static void test_bcache_stats_consistent(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b1 = bcache_get(id, 0);
    buf_t *b2 = bcache_get(id, 1);
    bcache_dirty(b1);

    bcache_stats_t st;
    bcache_get_stats(&st);

    /* valid + free should equal total. */
    ASSERT_EQ(dev, st.valid_buffers + st.free_buffers,
              st.total_buffers);
    /* dirty should be at least 1. */
    ASSERT_TRUE(dev, st.dirty_buffers >= 1);

    bcache_put(b1);
    bcache_put(b2);
}

/** E4: Cache miss on invalid sector returns NULL. */
static void test_bcache_get_invalid_sector(serial_dev_t *dev) {
    bcache_init(dev);
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    /* Sector MOCK_SECTORS is out of range for the mock device. */
    buf_t *b = bcache_get(id, MOCK_SECTORS + 100);
    ASSERT_NULL(dev, b);
}

/** E5: hash_misses increments on first access to a block. */
static void test_bcache_hash_miss_counting(serial_dev_t *dev) {
    bcache_init(dev);
    bcache_reset_stats();
    mock_reset();
    block_dev_unregister(0);
    uint8_t id = mock_register();

    buf_t *b = bcache_get(id, 0);
    ASSERT_NOT_NULL(dev, b);

    bcache_stats_t st;
    bcache_get_stats(&st);
    t_log(dev, "hash_miss", st.hash_misses);
    ASSERT_EQ(dev, st.hash_misses, 1U);
    ASSERT_EQ(dev, st.hash_hits, 0U);

    /* Second access is a hit. */
    buf_t *b2 = bcache_get(id, 0);
    ASSERT_NOT_NULL(dev, b2);

    bcache_get_stats(&st);
    t_log(dev, "hash_hit", st.hash_hits);
    ASSERT_EQ(dev, st.hash_hits, 1U);

    bcache_put(b);
    bcache_put(b2);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_bcache(void) {
    /* Group A — Init / Lifecycle */
    test_register("bc_init_total",       test_bcache_init_total);
    test_register("bc_init_free",        test_bcache_init_free);
    test_register("bc_init_zero",        test_bcache_init_zero_counts);
    test_register("bc_reset_stats",      test_bcache_reset_stats);
    test_register("bc_init_reinit",      test_bcache_init_reinit);

    /* Group B — Read-through */
    test_register("bc_get_read_data",    test_bcache_get_read_data);
    test_register("bc_get_same_buf",     test_bcache_get_same_buffer);
    test_register("bc_get_refcount",     test_bcache_get_refcount);
    test_register("bc_put_decrements",   test_bcache_put_decrements);
    test_register("bc_get_diff_blocks",  test_bcache_get_different_blocks);
    test_register("bc_get_disk_io",      test_bcache_get_disk_io);

    /* Group C — Dirty / Write-back */
    test_register("bc_dirty_flag",       test_bcache_dirty_flag);
    test_register("bc_flush_writes",     test_bcache_flush_writes);
    test_register("bc_flush_clr_dirty",  test_bcache_flush_clears_dirty);
    test_register("bc_flush_skips_cln",  test_bcache_flush_skips_clean);
    test_register("bc_flush_all",        test_bcache_flush_all);

    /* Group D — Eviction / LRU / Invalidation */
    test_register("bc_evict_dirty_wb",   test_bcache_evict_dirty_writeback);
    test_register("bc_evict_clean_no",   test_bcache_evict_clean_no_write);
    test_register("bc_no_evict_pinned",  test_bcache_no_evict_pinned);
    test_register("bc_inv_dev",          test_bcache_invalidate_dev);
    test_register("bc_inv_all",          test_bcache_invalidate_all);

    /* Group E — Edge cases and statistics */
    test_register("bc_put_null",         test_bcache_put_null);
    test_register("bc_dirty_null",       test_bcache_dirty_null);
    test_register("bc_stats_consistent", test_bcache_stats_consistent);
    test_register("bc_get_invalid_sec",  test_bcache_get_invalid_sector);
    test_register("bc_hash_miss_cnt",    test_bcache_hash_miss_counting);
}
