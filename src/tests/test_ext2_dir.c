/**
 * @file test_ext2_dir.c
 * @brief ext2 directory operation tests.
 *
 * Tests:
 *   Group N — Directory Operations (lookup, add, remove, mkdir)
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
 * Create an empty directory inode (no data blocks, size=0).
 * Returns the inode number.
 */
static uint32_t create_empty_dir(void) {
    int32_t ino = ext2_alloc_inode();
    if (ino < 0) return 0;
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = EXT2_S_IFDIR | 0755;
    inode.i_links_count = 1;
    ext2_write_inode((uint32_t)ino, &inode);
    return (uint32_t)ino;
}

/* =========================================================================
 * Group N — Directory Operations
 * ========================================================================= */

static void test_dir_lookup_root(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    /* Root directory has "." → inode 2. */
    uint32_t r = ext2_dir_lookup(EXT2_ROOT_INO, ".");
    ASSERT_EQ(dev, (int)r, (int)EXT2_ROOT_INO);

    ext2_unmount();
}

static void test_dir_lookup_notfound(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t r = ext2_dir_lookup(EXT2_ROOT_INO, "nonexistent");
    ASSERT_EQ(dev, (int)r, 0);

    ext2_unmount();
}

static void test_dir_add_one(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    /* Create an empty directory inode and add one entry. */
    uint32_t dir_ino = create_empty_dir();
    ASSERT_NEQ(dev, (int)dir_ino, 0);

    int32_t child_ino = ext2_alloc_inode();
    ASSERT_NEQ(dev, (int)child_ino, -1);

    ext2_status_t st = ext2_dir_add_entry(dir_ino, "foo.txt",
                                           (uint32_t)child_ino,
                                           EXT2_FT_REG_FILE);
    ASSERT_EQ(dev, (int)st, (int)EXT2_OK);

    /* Lookup should return the child inode. */
    uint32_t found = ext2_dir_lookup(dir_ino, "foo.txt");
    ASSERT_EQ(dev, (int)found, (int)child_ino);

    ext2_unmount();
}

static void test_dir_add_multi(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    uint32_t dir_ino = create_empty_dir();

    /* Add 3 entries. */
    int32_t ino_a = ext2_alloc_inode();
    int32_t ino_b = ext2_alloc_inode();
    int32_t ino_c = ext2_alloc_inode();

    ext2_dir_add_entry(dir_ino, "a",   (uint32_t)ino_a, EXT2_FT_REG_FILE);
    ext2_dir_add_entry(dir_ino, "bb",  (uint32_t)ino_b, EXT2_FT_REG_FILE);
    ext2_dir_add_entry(dir_ino, "ccc", (uint32_t)ino_c, EXT2_FT_REG_FILE);

    /* All three should be findable. */
    ASSERT_EQ(dev, (int)ext2_dir_lookup(dir_ino, "a"),   (int)ino_a);
    ASSERT_EQ(dev, (int)ext2_dir_lookup(dir_ino, "bb"),  (int)ino_b);
    ASSERT_EQ(dev, (int)ext2_dir_lookup(dir_ino, "ccc"), (int)ino_c);

    /* Non-existent name returns 0. */
    ASSERT_EQ(dev, (int)ext2_dir_lookup(dir_ino, "ddd"), 0);

    ext2_unmount();
}

static void test_dir_remove(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    uint32_t dir_ino = create_empty_dir();
    int32_t child_ino = ext2_alloc_inode();

    ext2_dir_add_entry(dir_ino, "foo.txt", (uint32_t)child_ino, EXT2_FT_REG_FILE);
    ASSERT_NEQ(dev, (int)ext2_dir_lookup(dir_ino, "foo.txt"), 0);

    ext2_status_t st = ext2_dir_remove_entry(dir_ino, "foo.txt");
    ASSERT_EQ(dev, (int)st, (int)EXT2_OK);

    /* Should no longer be found. */
    ASSERT_EQ(dev, (int)ext2_dir_lookup(dir_ino, "foo.txt"), 0);

    ext2_unmount();
}

static void test_mkdir_basic(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t sub_ino = ext2_mkdir(EXT2_ROOT_INO, "subdir");
    ASSERT_NEQ(dev, (int)sub_ino, 0);

    /* Read the new directory's inode — should be a directory. */
    ext2_inode_t sub_inode;
    ext2_read_inode(sub_ino, &sub_inode);
    ASSERT_EQ(dev, (int)(sub_inode.i_mode & EXT2_S_IFDIR), (int)EXT2_S_IFDIR);

    /* Read the data block and check "." and ".." entries. */
    uint32_t phys = ext2_inode_get_block(&sub_inode, 0);
    ASSERT_NEQ(dev, (int)phys, 0);

    uint8_t block_buf[1024];
    ext2_read_block(phys, block_buf);

    ext2_dirent_t *de_dot = (ext2_dirent_t *)block_buf;
    ASSERT_EQ(dev, (int)de_dot->inode, (int)sub_ino);
    ASSERT_EQ(dev, (int)de_dot->name_len, 1);
    ASSERT_EQ(dev, (int)de_dot->name[0], '.');

    ext2_dirent_t *de_dotdot = (ext2_dirent_t *)(block_buf + de_dot->rec_len);
    ASSERT_EQ(dev, (int)de_dotdot->inode, (int)EXT2_ROOT_INO);
    ASSERT_EQ(dev, (int)de_dotdot->name_len, 2);
    ASSERT_EQ(dev, (int)de_dotdot->name[0], '.');
    ASSERT_EQ(dev, (int)de_dotdot->name[1], '.');

    ext2_unmount();
}

static void test_mkdir_links(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    /* Read parent's link count before. */
    ext2_inode_t pinode_before;
    ext2_read_inode(EXT2_ROOT_INO, &pinode_before);
    uint16_t before_links = pinode_before.i_links_count;

    ext2_mkdir(EXT2_ROOT_INO, "subdir");

    /* Parent's link count should have increased by 1. */
    ext2_inode_t pinode_after;
    ext2_read_inode(EXT2_ROOT_INO, &pinode_after);
    ASSERT_EQ(dev, (int)pinode_after.i_links_count, (int)(before_links + 1));

    ext2_unmount();
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ext2_dir(void) {
    mock_register_once();

    test_register("ext2_dir_lk_root",  test_dir_lookup_root);
    test_register("ext2_dir_lk_miss",  test_dir_lookup_notfound);
    test_register("ext2_dir_add1",     test_dir_add_one);
    test_register("ext2_dir_add3",     test_dir_add_multi);
    test_register("ext2_dir_rm",       test_dir_remove);
    test_register("ext2_mkdir_basic",  test_mkdir_basic);
    test_register("ext2_mkdir_links",  test_mkdir_links);
}
