/**
 * @file test_ext2_path.c
 * @brief ext2 path resolution tests.
 *
 * Tests:
 *   Group O — Path Resolution (resolve_path, split_path)
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

/* =========================================================================
 * Group O — Path Resolution
 * ========================================================================= */

static void test_resolve_root(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t r = ext2_resolve_path("/");
    ASSERT_EQ(dev, (int)r, (int)EXT2_ROOT_INO);

    ext2_unmount();
}

static void test_resolve_single(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t sub = ext2_mkdir(EXT2_ROOT_INO, "foo");
    ASSERT_NEQ(dev, (int)sub, 0);

    uint32_t r = ext2_resolve_path("/foo");
    ASSERT_EQ(dev, (int)r, (int)sub);

    ext2_unmount();
}

static void test_resolve_nested(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t a = ext2_mkdir(EXT2_ROOT_INO, "a");
    uint32_t b = ext2_mkdir(a, "b");

    uint32_t r = ext2_resolve_path("/a/b");
    ASSERT_EQ(dev, (int)r, (int)b);

    ext2_unmount();
}

static void test_resolve_notfound(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t r = ext2_resolve_path("/nonexistent");
    ASSERT_EQ(dev, (int)r, 0);

    ext2_unmount();
}

static void test_split_root(serial_dev_t *dev) {
    char parent[EXT2_MAX_PATH];
    char name[EXT2_MAX_PATH];

    ext2_split_path("/", parent, name);
    ASSERT_EQ(dev, (int)parent[0], '/');
    ASSERT_EQ(dev, (int)parent[1], '\0');
    ASSERT_EQ(dev, (int)name[0], '\0');
}

static void test_split_deep(serial_dev_t *dev) {
    char parent[EXT2_MAX_PATH];
    char name[EXT2_MAX_PATH];

    ext2_split_path("/foo/bar/baz", parent, name);
    ASSERT_EQ(dev, (int)strcmp(parent, "/foo/bar"), 0);
    ASSERT_EQ(dev, (int)strcmp(name, "baz"), 0);
}

static void test_split_flat(serial_dev_t *dev) {
    char parent[EXT2_MAX_PATH];
    char name[EXT2_MAX_PATH];

    ext2_split_path("file.txt", parent, name);
    ASSERT_EQ(dev, (int)parent[0], '/');
    ASSERT_EQ(dev, (int)parent[1], '\0');
    ASSERT_EQ(dev, (int)strcmp(name, "file.txt"), 0);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ext2_path(void) {
    mock_register_once();

    test_register("ext2_res_root",   test_resolve_root);
    test_register("ext2_res_single", test_resolve_single);
    test_register("ext2_res_nested", test_resolve_nested);
    test_register("ext2_res_miss",   test_resolve_notfound);
    test_register("ext2_spl_root",   test_split_root);
    test_register("ext2_spl_deep",   test_split_deep);
    test_register("ext2_spl_flat",   test_split_flat);
}
