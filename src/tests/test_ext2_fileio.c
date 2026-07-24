/**
 * @file test_ext2_fileio.c
 * @brief ext2 file read/write tests.
 *
 * Tests:
 *   Group M — File I/O (read_file, write_file)
 */

#include "test.h"
#include "../fs/ext2.h"
#include "../fs/bcache.h"
#include "../fs/block_dev.h"
#include "../kernel/kheap.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Mock Device — RAM-backed ext2 image
 * ========================================================================= */

#define MOCK_SECTORS      256
#define MOCK_SECTOR_SIZE  512
#define MOCK_EXT2_BLOCK   1024

static uint8_t mock_storage[MOCK_SECTORS * MOCK_SECTOR_SIZE];

static block_status_t mock_read_fn(uint8_t dev_id, uint32_t lba, void *buf) {
    (void)dev_id;
    if (lba >= MOCK_SECTORS) return BLOCK_ERR_INVALID;
    memcpy(buf, &mock_storage[lba * MOCK_SECTOR_SIZE], MOCK_SECTOR_SIZE);
    return BLOCK_OK;
}

static block_status_t mock_write_fn(uint8_t dev_id, uint32_t lba, const void *buf) {
    (void)dev_id;
    if (lba >= MOCK_SECTORS) return BLOCK_ERR_INVALID;
    memcpy(&mock_storage[lba * MOCK_SECTOR_SIZE], buf, MOCK_SECTOR_SIZE);
    return BLOCK_OK;
}

static void mock_reset(void) {
    memset(mock_storage, 0, sizeof(mock_storage));
}

static uint8_t g_mock_dev_id = 0xFF;

static void mock_register_once(void) {
    if (g_mock_dev_id != 0xFF) {
        block_dev_unregister(g_mock_dev_id);
        bcache_invalidate_dev(g_mock_dev_id);
        g_mock_dev_id = 0xFF;
    }
    block_device_t dev = {
        .read_sector  = mock_read_fn,
        .write_sector = mock_write_fn,
        .sector_size  = MOCK_SECTOR_SIZE,
        .total_sectors = MOCK_SECTORS,
    };
    block_dev_register(&dev, &g_mock_dev_id);
}

static void mock_format_ext2(void) {
    memset(mock_storage, 0, sizeof(mock_storage));

    ext2_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count      = 4096;
    sb.s_blocks_count      = 256;
    sb.s_free_blocks_count = 243;
    sb.s_free_inodes_count = 4094;
    sb.s_first_data_block  = 1;
    sb.s_log_block_size    = 0;
    sb.s_blocks_per_group  = 256;
    sb.s_frags_per_group   = 256;
    sb.s_inodes_per_group  = 4096;
    sb.s_mnt_count         = 1;
    sb.s_max_mnt_count     = -1;
    sb.s_magic             = EXT2_SUPER_MAGIC;
    sb.s_state             = EXT2_FS_CLEAN;
    sb.s_errors            = EXT2_ERRORS_CONTINUE;
    sb.s_rev_level         = 1;
    sb.s_first_ino         = 11;
    sb.s_inode_size        = 128;
    memset(sb.s_uuid, 0xAB, 16);
    memcpy(sb.s_volume_name, "test", 5);
    memcpy(&mock_storage[2 * MOCK_SECTOR_SIZE], &sb, sizeof(sb));

    ext2_group_desc_t gd;
    memset(&gd, 0, sizeof(gd));
    gd.bg_block_bitmap      = 3;
    gd.bg_inode_bitmap      = 4;
    gd.bg_inode_table       = 5;
    gd.bg_free_blocks_count = 243;
    gd.bg_free_inodes_count = 4094;
    memcpy(&mock_storage[4 * MOCK_SECTOR_SIZE], &gd, sizeof(gd));

    mock_storage[6 * MOCK_SECTOR_SIZE]     = 0xFF;
    mock_storage[6 * MOCK_SECTOR_SIZE + 1] = 0x1F;
    mock_storage[8 * MOCK_SECTOR_SIZE] = 0xFF;
}

static void ext2_test_setup(void) {
    mock_reset();
    mock_format_ext2();
    mock_register_once();
    bcache_invalidate_all();
    ext2_init(NULL);
}

/**
 * Create a fresh regular file inode on disk with the given inode number.
 * Returns the inode via @p out. The inode has i_links_count=1, mode=REG.
 */
static void create_file_inode(uint32_t ino, ext2_inode_t *out) {
    memset(out, 0, sizeof(*out));
    out->i_mode        = EXT2_S_IFREG | 0644;
    out->i_links_count = 1;
    ext2_write_inode(ino, out);
}

/* =========================================================================
 * Group M — File I/O
 * ========================================================================= */

static void test_read_empty_file(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    /* Create inode 11 as an empty regular file. */
    ext2_inode_t ino;
    create_file_inode(11, &ino);

    uint8_t buf[64];
    int64_t n = ext2_read_file(11, buf, 0, sizeof(buf));
    ASSERT_EQ(dev, (int)n, 0);

    ext2_unmount();
}

static void test_write_read_1block(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    create_file_inode(11, &ino);

    /* Write 100 bytes at offset 0. */
    uint8_t wdata[100];
    for (int i = 0; i < 100; i++) wdata[i] = (uint8_t)(i + 0x10);

    int64_t written = ext2_write_file(11, wdata, 0, 100);
    ASSERT_EQ(dev, (int)written, 100);

    /* Read back. */
    uint8_t rdata[100];
    int64_t n = ext2_read_file(11, rdata, 0, 100);
    ASSERT_EQ(dev, (int)n, 100);

    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(dev, rdata[i], (uint8_t)(i + 0x10));
    }

    ext2_unmount();
}

static void test_write_read_multiblock(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    create_file_inode(11, &ino);

    /* Write 3000 bytes — spans 3 blocks (1024 + 1024 + 952). */
    uint8_t wdata[3000];
    for (int i = 0; i < 3000; i++) wdata[i] = (uint8_t)(i & 0xFF);

    int64_t written = ext2_write_file(11, wdata, 0, 3000);
    ASSERT_EQ(dev, (int)written, 3000);

    /* Read back all. */
    uint8_t rdata[3000];
    memset(rdata, 0, sizeof(rdata));
    int64_t n = ext2_read_file(11, rdata, 0, 3000);
    ASSERT_EQ(dev, (int)n, 3000);

    for (int i = 0; i < 3000; i++) {
        if (rdata[i] != (uint8_t)(i & 0xFF)) {
            /* Fail at first mismatch. */
            ASSERT_EQ(dev, rdata[i], (uint8_t)(i & 0xFF));
        }
    }

    ext2_unmount();
}

static void test_write_read_offset(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    create_file_inode(11, &ino);

    /* Write 100 bytes at offset 500. */
    uint8_t wdata[100];
    memset(wdata, 0xAB, 100);

    int64_t written = ext2_write_file(11, wdata, 500, 100);
    ASSERT_EQ(dev, (int)written, 100);

    /* Read back at offset 500. */
    uint8_t rdata[100];
    int64_t n = ext2_read_file(11, rdata, 500, 100);
    ASSERT_EQ(dev, (int)n, 100);
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(dev, rdata[i], 0xAB);
    }

    ext2_unmount();
}

static void test_write_extends_size(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    create_file_inode(11, &ino);

    /* Write 10 bytes at offset 2000 — creates a sparse file. */
    uint8_t wdata[10];
    memset(wdata, 0xCC, 10);

    int64_t written = ext2_write_file(11, wdata, 2000, 10);
    ASSERT_EQ(dev, (int)written, 10);

    /* Verify i_size was updated. */
    ext2_inode_t ino2;
    ext2_read_inode(11, &ino2);
    ASSERT_EQ(dev, (int)ino2.i_size, 2010);

    ext2_unmount();
}

static void test_read_beyond_size(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    create_file_inode(11, &ino);

    /* Write 50 bytes. */
    uint8_t wdata[50];
    memset(wdata, 0xDD, 50);
    ext2_write_file(11, wdata, 0, 50);

    /* Read past end — should return 0. */
    uint8_t rdata[64];
    int64_t n = ext2_read_file(11, rdata, 100, 64);
    ASSERT_EQ(dev, (int)n, 0);

    ext2_unmount();
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ext2_fileio(void) {
    mock_register_once();

    test_register("ext2_read_empty",     test_read_empty_file);
    test_register("ext2_wr_rd_1blk",    test_write_read_1block);
    test_register("ext2_wr_rd_multi",   test_write_read_multiblock);
    test_register("ext2_wr_rd_offset",  test_write_read_offset);
    test_register("ext2_wr_ext_size",   test_write_extends_size);
    test_register("ext2_rd_past_end",   test_read_beyond_size);
}
