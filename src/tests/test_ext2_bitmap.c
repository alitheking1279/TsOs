/**
 * @file test_ext2_bitmap.c
 * @brief ext2 bitmap allocator tests.
 *
 * Tests:
 *   Group K — Bitmap allocator (block and inode alloc/free)
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
 * Format mock_storage as a valid ext2 image.
 *
 * Blocks 0-12 marked used (boot, sb, gd, bitmaps, inode table).
 * Inodes 1-8 marked used (bad blocks inode + padding).
 * Free blocks: 243 (blocks 13-255). Free inodes: 4094 (9-4096).
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
    sb.s_log_block_size    = 0;
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
    gd.bg_used_dirs_count   = 0;

    memcpy(&mock_storage[4 * MOCK_SECTOR_SIZE], &gd, sizeof(gd));

    /* Mark blocks 0-12 as used in the block bitmap (block 3, sector 6) */
    mock_storage[6 * MOCK_SECTOR_SIZE] = 0xFF;     /* bits 0-7 */
    mock_storage[6 * MOCK_SECTOR_SIZE + 1] = 0x1F; /* bits 8-12 */

    /* Mark inodes 1-8 as used in the inode bitmap (block 4, sector 8) */
    mock_storage[8 * MOCK_SECTOR_SIZE] = 0xFF;
}

static void ext2_test_setup(void) {
    mock_reset();
    mock_format_ext2();
    mock_register_once();
    bcache_invalidate_all();
    ext2_init(NULL);
}

/* =========================================================================
 * Group K — Bitmap allocator
 * ========================================================================= */

static void test_alloc_block_basic(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    /* Blocks 0-12 are used. First free = 13. */
    int32_t b = ext2_alloc_block();
    ASSERT_EQ(dev, (int)b, 13);

    ext2_unmount();
}

static void test_alloc_block_sequential(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    int32_t b1 = ext2_alloc_block();
    int32_t b2 = ext2_alloc_block();
    int32_t b3 = ext2_alloc_block();
    ASSERT_EQ(dev, (int)b1, 13);
    ASSERT_EQ(dev, (int)b2, 14);
    ASSERT_EQ(dev, (int)b3, 15);

    ext2_unmount();
}

static void test_free_block(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    int32_t b = ext2_alloc_block();
    ASSERT_EQ(dev, (int)b, 13);

    ext2_status_t st = ext2_free_block((uint32_t)b);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    /* Alloc again — should get the same block back. */
    int32_t b2 = ext2_alloc_block();
    ASSERT_EQ(dev, (int)b2, 13);

    ext2_unmount();
}

static void test_alloc_inode_basic(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    /* Inodes 1-8 are used. First free = 9. */
    int32_t ino = ext2_alloc_inode();
    ASSERT_EQ(dev, (int)ino, 9);

    ext2_unmount();
}

static void test_free_inode(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    int32_t ino = ext2_alloc_inode();
    ASSERT_EQ(dev, (int)ino, 9);

    ext2_status_t st = ext2_free_inode((uint32_t)ino);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    /* Alloc again — should get inode 9 back. */
    int32_t ino2 = ext2_alloc_inode();
    ASSERT_EQ(dev, (int)ino2, 9);

    ext2_unmount();
}

static void test_alloc_exhaust_blocks(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    /* Allocate all 243 free blocks (13-255). */
    for (int i = 0; i < 243; i++) {
        int32_t b = ext2_alloc_block();
        ASSERT_NEQ(dev, (int)b, -1);
    }

    /* Next alloc should fail. */
    int32_t b = ext2_alloc_block();
    ASSERT_EQ(dev, (int)b, -1);

    ext2_unmount();
}

static void test_alloc_updates_superblock(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_fs_t *fs = ext2_get_fs();
    uint32_t before = fs->sb.s_free_blocks_count;

    ext2_alloc_block();

    ASSERT_EQ(dev, (int)fs->sb.s_free_blocks_count, (int)before - 1);

    ext2_unmount();
}

static void test_alloc_updates_group_desc(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_fs_t *fs = ext2_get_fs();
    uint16_t before = fs->gd[0].bg_free_blocks_count;

    ext2_alloc_block();

    ASSERT_EQ(dev, (int)fs->gd[0].bg_free_blocks_count, (int)before - 1);

    ext2_unmount();
}

static void test_free_restores_counts(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_fs_t *fs = ext2_get_fs();
    uint32_t sb_before   = fs->sb.s_free_blocks_count;
    uint16_t gd_before   = fs->gd[0].bg_free_blocks_count;

    int32_t b = ext2_alloc_block();
    ASSERT_NEQ(dev, (int)b, -1);

    ext2_free_block((uint32_t)b);

    ASSERT_EQ(dev, (int)fs->sb.s_free_blocks_count, (int)sb_before);
    ASSERT_EQ(dev, (int)fs->gd[0].bg_free_blocks_count, (int)gd_before);

    ext2_unmount();
}

static void test_init_root_uses_alloc(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_fs_t *fs = ext2_get_fs();
    uint32_t sb_before = fs->sb.s_free_blocks_count;

    ext2_status_t st = ext2_init_root_dir();
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    /* One block was allocated for the root data. */
    ASSERT_EQ(dev, (int)fs->sb.s_free_blocks_count, (int)sb_before - 1);

    /* Root inode should exist and point to a data block. */
    ext2_inode_t root_ino;
    st = ext2_read_inode(EXT2_ROOT_INO, &root_ino);
    ASSERT_EQ(dev, (int)st, EXT2_OK);
    ASSERT_EQ(dev, (int)(root_ino.i_mode & 0xF000), EXT2_S_IFDIR);
    ASSERT_NEQ(dev, (int)root_ino.i_block[0], 0);

    /* Directory entries should be valid. */
    uint8_t dir_buf[1024];
    st = ext2_read_block(root_ino.i_block[0], dir_buf);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    ext2_dirent_t *dot = (ext2_dirent_t *)dir_buf;
    ASSERT_EQ(dev, dot->inode, 2);
    ASSERT_EQ(dev, dot->name_len, 1);
    ASSERT_EQ(dev, dot->name[0], '.');

    ext2_dirent_t *dotdot = (ext2_dirent_t *)(dir_buf + 12);
    ASSERT_EQ(dev, dotdot->inode, 2);
    ASSERT_EQ(dev, dotdot->name_len, 2);
    ASSERT_EQ(dev, dotdot->name[0], '.');
    ASSERT_EQ(dev, dotdot->name[1], '.');

    ext2_unmount();
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ext2_bitmap(void) {
    mock_register_once();

    /* Group K — bitmap allocator */
    test_register("ext2_alloc_blk",        test_alloc_block_basic);
    test_register("ext2_alloc_blk_seq",    test_alloc_block_sequential);
    test_register("ext2_free_blk",         test_free_block);
    test_register("ext2_alloc_ino",        test_alloc_inode_basic);
    test_register("ext2_free_ino",         test_free_inode);
    test_register("ext2_alloc_exhaust",    test_alloc_exhaust_blocks);
    test_register("ext2_alloc_sb_upd",     test_alloc_updates_superblock);
    test_register("ext2_alloc_gd_upd",     test_alloc_updates_group_desc);
    test_register("ext2_free_restore",     test_free_restores_counts);
    test_register("ext2_init_root_alloc",  test_init_root_uses_alloc);
}
