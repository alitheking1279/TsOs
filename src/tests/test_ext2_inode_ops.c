/**
 * @file test_ext2_inode_ops.c
 * @brief ext2 inode read/write operation tests.
 *
 * Tests:
 *   Group J — Inode read/write operations
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
 * Mock Device — RAM-backed ext2 image (same as test_ext2_struct.c)
 * ========================================================================= */

#define MOCK_SECTORS      256
#define MOCK_SECTOR_SIZE  512
#define MOCK_EXT2_BLOCK   1024

static uint8_t mock_storage[MOCK_SECTORS * MOCK_SECTOR_SIZE];
static uint32_t mock_read_count;
static uint32_t mock_write_count;

static block_status_t mock_read_fn(uint8_t dev_id, uint32_t lba, void *buf) {
    (void)dev_id;
    if (lba >= MOCK_SECTORS) return BLOCK_ERR_INVALID;
    memcpy(buf, &mock_storage[lba * MOCK_SECTOR_SIZE], MOCK_SECTOR_SIZE);
    mock_read_count++;
    return BLOCK_OK;
}

static block_status_t mock_write_fn(uint8_t dev_id, uint32_t lba, const void *buf) {
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

/**
 * Format mock_storage as a valid ext2 image with root directory initialized.
 */
static void mock_format_ext2(void) {
    memset(mock_storage, 0, sizeof(mock_storage));

    /* --- Superblock at block 1 (sector 2-3) --- */
    ext2_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count      = 4096;
    sb.s_blocks_count      = 256;
    sb.s_r_blocks_count    = 0;
    sb.s_free_blocks_count = 243;
    sb.s_free_inodes_count = 4094;
    sb.s_first_data_block  = 1;
    sb.s_log_block_size    = 0;       /* 1024 */
    sb.s_log_frag_size     = 0;
    sb.s_blocks_per_group  = 256;
    sb.s_frags_per_group   = 256;
    sb.s_inodes_per_group  = 4096;
    sb.s_mtime             = 0;
    sb.s_wtime             = 0;
    sb.s_mnt_count         = 1;
    sb.s_max_mnt_count     = -1;
    sb.s_magic             = EXT2_SUPER_MAGIC;
    sb.s_state             = EXT2_FS_CLEAN;
    sb.s_errors            = EXT2_ERRORS_CONTINUE;
    sb.s_minor_rev_level   = 0;
    sb.s_lastcheck         = 0;
    sb.s_checkinterval     = 0;
    sb.s_creator_os        = 0;
    sb.s_rev_level         = 1;
    sb.s_first_ino         = 11;
    sb.s_inode_size        = 128;
    sb.s_block_group_nr    = 0;
    sb.s_feature_compat    = 0;
    sb.s_feature_incompat  = 0;
    sb.s_feature_ro_compat = 0;
    memset(sb.s_uuid, 0xAB, 16);
    memcpy(sb.s_volume_name, "test", 5);
    memcpy(sb.s_last_mounted, "/mnt", 5);

    memcpy(&mock_storage[2 * MOCK_SECTOR_SIZE], &sb, sizeof(sb));

    /* --- Group descriptor at block 2 (sectors 4-5) --- */
    ext2_group_desc_t gd;
    memset(&gd, 0, sizeof(gd));
    gd.bg_block_bitmap      = 3;
    gd.bg_inode_bitmap      = 4;
    gd.bg_inode_table       = 5;
    gd.bg_free_blocks_count = 243;
    gd.bg_free_inodes_count = 4094;
    gd.bg_used_dirs_count   = 1;

    memcpy(&mock_storage[4 * MOCK_SECTOR_SIZE], &gd, sizeof(gd));

    /* Mark blocks 0-12 as used in the block bitmap (block 3, sector 6) */
    mock_storage[6 * MOCK_SECTOR_SIZE] = 0xFF;     /* bits 0-7 */
    mock_storage[6 * MOCK_SECTOR_SIZE + 1] = 0x1F; /* bits 8-12 */

    /* Mark inodes 1-8 as used in the inode bitmap (block 4, sector 8) */
    mock_storage[8 * MOCK_SECTOR_SIZE] = 0xFF;

    /* --- Root directory inode (inode 2) in the inode table --- */
    /* Inode table starts at block 5 (sector 10). Inode 2 is at byte offset
     * (2-1) * 128 = 128 within the inode table. */
    ext2_inode_t root_ino;
    memset(&root_ino, 0, sizeof(root_ino));
    root_ino.i_mode        = EXT2_S_IFDIR | 0755;
    root_ino.i_links_count = 2;
    root_ino.i_blocks      = 2;  /* 1024 / 512 = 2 x 512-byte sectors */
    root_ino.i_size        = 1024;
    root_ino.i_block[0]    = 13; /* first data block */

    memcpy(&mock_storage[10 * MOCK_SECTOR_SIZE + 128], &root_ino, sizeof(root_ino));

    /* --- Root directory entries in block 13 (sector 26-27) --- */
    uint8_t dir_buf[1024];
    memset(dir_buf, 0, sizeof(dir_buf));

    ext2_dirent_t *de = (ext2_dirent_t *)dir_buf;
    de->inode     = 2;
    de->rec_len   = 12;
    de->name_len  = 1;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';

    de = (ext2_dirent_t *)(dir_buf + 12);
    de->inode     = 2;
    de->rec_len   = 1024 - 12;
    de->name_len  = 2;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';
    de->name[1]   = '.';

    memcpy(&mock_storage[26 * MOCK_SECTOR_SIZE], dir_buf, 1024);
}

static void ext2_test_setup(void) {
    mock_reset();
    mock_format_ext2();
    mock_register_once();
    bcache_invalidate_all();
    ext2_init(NULL);
}

/* =========================================================================
 * Group J — Inode read/write operations
 * ========================================================================= */

static void test_read_root_inode(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    ext2_status_t st = ext2_read_inode(EXT2_ROOT_INO, &ino);
    ASSERT_EQ(dev, (int)st, EXT2_OK);
    ASSERT_EQ(dev, (int)(ino.i_mode & 0xF000), EXT2_S_IFDIR);
    ASSERT_EQ(dev, (int)(ino.i_mode & 0777), 0755);
    ASSERT_EQ(dev, (int)ino.i_links_count, 2);
    ASSERT_EQ(dev, (int)ino.i_block[0], 13);
    ASSERT_EQ(dev, (int)ino.i_size, 1024);

    ext2_unmount();
}

static void test_read_ino1_badblocks(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    ext2_status_t st = ext2_read_inode(1, &ino);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    /* Inode 1 (bad blocks) should be all zeros in a fresh image. */
    ASSERT_EQ(dev, (int)ino.i_mode, 0);
    ASSERT_EQ(dev, (int)ino.i_links_count, 0);

    ext2_unmount();
}

static void test_write_read_inode(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    ext2_status_t st = ext2_read_inode(EXT2_ROOT_INO, &ino);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    /* Modify size and write back. */
    ino.i_size = 2048;
    st = ext2_write_inode(EXT2_ROOT_INO, &ino);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    /* Read back and verify. */
    ext2_inode_t ino2;
    st = ext2_read_inode(EXT2_ROOT_INO, &ino2);
    ASSERT_EQ(dev, (int)st, EXT2_OK);
    ASSERT_EQ(dev, (int)ino2.i_size, 2048);

    /* Original fields should be preserved. */
    ASSERT_EQ(dev, (int)(ino2.i_mode & 0xF000), EXT2_S_IFDIR);
    ASSERT_EQ(dev, (int)ino2.i_links_count, 2);
    ASSERT_EQ(dev, (int)ino2.i_block[0], 13);

    ext2_unmount();
}

static void test_read_ino_out_of_range(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    ext2_status_t st = ext2_read_inode(99999, &ino);
    ASSERT_EQ(dev, (int)st, EXT2_ERR_INVALID);

    ext2_unmount();
}

static void test_write_ino_not_mounted(serial_dev_t *dev) {
    ext2_init(NULL);
    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ext2_status_t st = ext2_write_inode(2, &ino);
    ASSERT_EQ(dev, (int)st, EXT2_ERR_NOT_MOUNTED);
}

static void test_read_ino_null(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_status_t st = ext2_read_inode(2, NULL);
    ASSERT_EQ(dev, (int)st, EXT2_ERR_INVALID);

    ext2_unmount();
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ext2_inode_ops(void) {
    mock_register_once();

    /* Group J — inode read/write operations */
    test_register("ext2_read_root_ino",     test_read_root_inode);
    test_register("ext2_read_ino1_bb",      test_read_ino1_badblocks);
    test_register("ext2_write_read_ino",    test_write_read_inode);
    test_register("ext2_read_ino_oor",      test_read_ino_out_of_range);
    test_register("ext2_write_ino_nomnt",   test_write_ino_not_mounted);
    test_register("ext2_read_ino_null",     test_read_ino_null);
}
