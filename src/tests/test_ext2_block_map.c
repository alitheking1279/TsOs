/**
 * @file test_ext2_block_map.c
 * @brief ext2 inode block mapping tests.
 *
 * Tests:
 *   Group L — Inode block mapping (direct, indirect, alloc)
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

    /* Superblock */
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

    /* Group descriptor */
    ext2_group_desc_t gd;
    memset(&gd, 0, sizeof(gd));
    gd.bg_block_bitmap      = 3;
    gd.bg_inode_bitmap      = 4;
    gd.bg_inode_table       = 5;
    gd.bg_free_blocks_count = 243;
    gd.bg_free_inodes_count = 4094;
    memcpy(&mock_storage[4 * MOCK_SECTOR_SIZE], &gd, sizeof(gd));

    /* Block bitmap: blocks 0-12 used */
    mock_storage[6 * MOCK_SECTOR_SIZE]     = 0xFF;
    mock_storage[6 * MOCK_SECTOR_SIZE + 1] = 0x1F;

    /* Inode bitmap: inodes 1-8 used */
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
 * Group L — Inode block mapping
 * ========================================================================= */

static void test_get_direct(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    /* Create an inode with block[0] = 13, block[1] = 20. */
    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_block[0] = 13;
    ino.i_block[1] = 20;

    ASSERT_EQ(dev, (int)ext2_inode_get_block(&ino, 0), 13);
    ASSERT_EQ(dev, (int)ext2_inode_get_block(&ino, 1), 20);

    /* Unmapped direct returns 0. */
    ASSERT_EQ(dev, (int)ext2_inode_get_block(&ino, 2), 0);

    ext2_unmount();
}

static void test_get_single_indirect(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    /* Set up a single indirect block at block 50.
     * It contains 256 pointers; set pointer[0]=100, pointer[5]=200. */
    uint32_t ind[256];
    memset(ind, 0, sizeof(ind));
    ind[0] = 100;
    ind[5] = 200;
    ext2_write_block(50, ind);

    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_block[12] = 50;  /* single indirect pointer */

    /* logical 12 → ind[0] = 100 */
    ASSERT_EQ(dev, (int)ext2_inode_get_block(&ino, 12), 100);
    /* logical 17 → ind[5] = 200 */
    ASSERT_EQ(dev, (int)ext2_inode_get_block(&ino, 17), 200);
    /* Unmapped indirect entry returns 0. */
    ASSERT_EQ(dev, (int)ext2_inode_get_block(&ino, 13), 0);

    ext2_unmount();
}

static void test_get_double_indirect(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    /* Double indirect: i_block[13] → L1 block → L0 blocks → data.
     * For 1K blocks: ptrs=256. logical offset into double indirect:
     *   idx1 = logical / 256, idx0 = logical % 256
     * We'll test logical 268 (relative = 268 - 12 - 256 = 0):
     *   idx1=0, idx0=0 → L1[0] points to L0 block, L0[0]=phys.
     */
    uint32_t L0[256];
    memset(L0, 0, sizeof(L0));
    L0[0] = 300;
    ext2_write_block(60, L0);

    uint32_t L1[256];
    memset(L1, 0, sizeof(L1));
    L1[0] = 60;
    ext2_write_block(51, L1);

    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_block[13] = 51;

    /* logical 268 = 12 (direct) + 256 (single) + 0 (double) */
    ASSERT_EQ(dev, (int)ext2_inode_get_block(&ino, 268), 300);

    ext2_unmount();
}

static void test_alloc_direct(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_links_count = 1;

    ext2_status_t st = ext2_inode_alloc_block(2, &ino, 0);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    /* Block 13 should now be mapped in i_block[0]. */
    uint32_t phys = ext2_inode_get_block(&ino, 0);
    ASSERT_NEQ(dev, (int)phys, 0);

    /* Read the inode back from disk to verify persistence. */
    ext2_inode_t ino2;
    ext2_read_inode(2, &ino2);
    ASSERT_NEQ(dev, (int)ino2.i_block[0], 0);
    ASSERT_EQ(dev, (int)ino2.i_block[0], (int)phys);

    ext2_unmount();
}

static void test_alloc_indirect(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_links_count = 1;

    /* Allocate block at logical 12 (first single indirect). */
    ext2_status_t st = ext2_inode_alloc_block(2, &ino, 12);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    /* i_block[12] should now point to an indirect block. */
    ASSERT_NEQ(dev, (int)ino.i_block[12], 0);

    /* The indirect block should contain the allocated physical block. */
    uint32_t ind[256];
    ext2_read_block(ino.i_block[12], ind);
    ASSERT_NEQ(dev, (int)ind[0], 0);

    /* get_block should return the same physical block. */
    uint32_t phys = ext2_inode_get_block(&ino, 12);
    ASSERT_EQ(dev, (int)phys, (int)ind[0]);

    ext2_unmount();
}

static void test_get_null_inode(serial_dev_t *dev) {
    (void)dev;
    ASSERT_EQ(dev, (int)ext2_inode_get_block((void *)0, 0), 0);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ext2_block_map(void) {
    mock_register_once();

    test_register("ext2_get_direct",      test_get_direct);
    test_register("ext2_get_single_ind",  test_get_single_indirect);
    test_register("ext2_get_double_ind",  test_get_double_indirect);
    test_register("ext2_alloc_direct",    test_alloc_direct);
    test_register("ext2_alloc_indirect",  test_alloc_indirect);
    test_register("ext2_get_null",        test_get_null_inode);
}
