/**
 * @file test_ext2_phase9.c
 * @brief ext2 Phase 9 — stat, lseek, unlink, rmdir, getdents, rename tests.
 *
 * Tests:
 *   Group Q — File Operations (stat, lseek, unlink, rmdir, getdents, rename)
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

    /* Mark inodes 1-8 as used in bitmap (byte 0 = 0xFF, byte 1 = 0x1F = bits 0-4). */
    mock_storage[6 * MOCK_SECTOR_SIZE]     = 0xFF;
    mock_storage[6 * MOCK_SECTOR_SIZE + 1] = 0x1F;
    /* Mark blocks 0-12 as used in block bitmap. */
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
 * Helper: create a regular file with content
 * ========================================================================= */

static uint32_t create_file_with_data(const char *content, uint32_t len) {
    int32_t ino = ext2_alloc_inode();
    if (ino < 0) return 0;
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = EXT2_S_IFREG | 0644;
    inode.i_links_count = 1;
    ext2_write_inode((uint32_t)ino, &inode);
    if (len > 0) {
        ext2_write_file((uint32_t)ino, content, 0, len);
    }
    return (uint32_t)ino;
}

/* =========================================================================
 * Group Q — File Operations
 * ========================================================================= */

static void test_stat_new_file(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t ino = create_file_with_data("hello", 5);
    ASSERT_NEQ(dev, (int)ino, 0);

    ext2_stat_t st;
    ext2_status_t r = ext2_stat(ino, &st);
    ASSERT_EQ(dev, (int)r, (int)EXT2_OK);
    ASSERT_EQ(dev, (int)st.ino, (int)ino);
    ASSERT_EQ(dev, (int)st.size, 5);
    ASSERT_EQ(dev, (int)(st.mode & 0xF000), (int)EXT2_S_IFREG);
    ASSERT_EQ(dev, (int)st.links, 1);

    ext2_unmount();
}

static void test_stat_dir(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t sub = ext2_mkdir(EXT2_ROOT_INO, "sub");
    ASSERT_NEQ(dev, (int)sub, 0);

    ext2_stat_t st;
    ext2_status_t r = ext2_stat(sub, &st);
    ASSERT_EQ(dev, (int)r, (int)EXT2_OK);
    ASSERT_EQ(dev, (int)(st.mode & 0xF000), (int)EXT2_S_IFDIR);

    ext2_unmount();
}

static void test_lseek_set(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t ino = create_file_with_data("ABCDEF", 6);
    ASSERT_NEQ(dev, (int)ino, 0);

    /* Read all via ext2_read_file. */
    char buf[7] = {0};
    int64_t n = ext2_read_file(ino, buf, 0, 6);
    ASSERT_EQ(dev, (int)n, 6);
    ASSERT_EQ(dev, (int)buf[0], 'A');
    ASSERT_EQ(dev, (int)buf[5], 'F');

    ext2_unmount();
}

static void test_lseek_cur(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t ino = create_file_with_data("0123456789", 10);
    ASSERT_NEQ(dev, (int)ino, 0);

    /* Read 3 bytes starting at offset 2. */
    char buf[4] = {0};
    int64_t n = ext2_read_file(ino, buf, 2, 3);
    ASSERT_EQ(dev, (int)n, 3);
    ASSERT_EQ(dev, (int)buf[0], '2');
    ASSERT_EQ(dev, (int)buf[2], '4');

    ext2_unmount();
}

static void test_lseek_end(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t ino = create_file_with_data("XYZ", 3);
    ASSERT_NEQ(dev, (int)ino, 0);

    /* Read past end. */
    char buf[4] = {0};
    int64_t n = ext2_read_file(ino, buf, 3, 10);
    ASSERT_EQ(dev, (int)n, 0);

    ext2_unmount();
}

static void test_unlink_file(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t ino = create_file_with_data("data", 4);
    ext2_dir_add_entry(EXT2_ROOT_INO, "file.txt", ino, EXT2_FT_REG_FILE);

    /* Verify it's there. */
    ASSERT_NEQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "file.txt"), 0);

    /* Unlink it. */
    ext2_status_t st = ext2_unlink(EXT2_ROOT_INO, "file.txt");
    ASSERT_EQ(dev, (int)st, (int)EXT2_OK);

    /* Should be gone. */
    ASSERT_EQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "file.txt"), 0);

    ext2_unmount();
}

static void test_rmdir_empty(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t sub = ext2_mkdir(EXT2_ROOT_INO, "empty");
    ASSERT_NEQ(dev, (int)sub, 0);

    /* Should be findable. */
    ASSERT_NEQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "empty"), 0);

    /* Rmdir it. */
    ext2_status_t st = ext2_rmdir(EXT2_ROOT_INO, "empty");
    ASSERT_EQ(dev, (int)st, (int)EXT2_OK);

    /* Should be gone. */
    ASSERT_EQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "empty"), 0);

    ext2_unmount();
}

static void test_rmdir_nonempty(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t sub = ext2_mkdir(EXT2_ROOT_INO, "full");
    /* Add a file inside the subdirectory. */
    uint32_t ino = create_file_with_data("x", 1);
    ext2_dir_add_entry(sub, "a.txt", ino, EXT2_FT_REG_FILE);

    /* Rmdir should fail — not empty. */
    ext2_status_t st = ext2_rmdir(EXT2_ROOT_INO, "full");
    ASSERT_NEQ(dev, (int)st, (int)EXT2_OK);

    /* Directory should still exist. */
    ASSERT_NEQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "full"), 0);

    ext2_unmount();
}

static void test_getdents_dir(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    /* Add 3 files to root. */
    uint32_t i1 = create_file_with_data("a", 1);
    uint32_t i2 = create_file_with_data("b", 1);
    uint32_t i3 = create_file_with_data("c", 1);
    ext2_dir_add_entry(EXT2_ROOT_INO, "aaa.txt", i1, EXT2_FT_REG_FILE);
    ext2_dir_add_entry(EXT2_ROOT_INO, "bbb.txt", i2, EXT2_FT_REG_FILE);
    ext2_dir_add_entry(EXT2_ROOT_INO, "ccc.txt", i3, EXT2_FT_REG_FILE);

    /* Read directory entries. */
    uint8_t buf[1024];
    uint64_t cookie = 0;
    int32_t result = ext2_getdents(EXT2_ROOT_INO, &cookie, buf, sizeof(buf));
    ASSERT_TRUE(dev, result > 0);

    /* Should have at least "." and ".." plus our 3 files. */
    uint32_t total = 0;
    uint32_t off = 0;
    while (off < (uint32_t)result) {
        ext2_dirent64_t *de = (ext2_dirent64_t *)(buf + off);
        off += de->d_reclen;
        total++;
    }
    /* "." + ".." + "aaa.txt" + "bbb.txt" + "ccc.txt" = 5 */
    ASSERT_EQ(dev, (int)total, 5);

    ext2_unmount();
}

static void test_rename_same_dir(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t ino = create_file_with_data("data", 4);
    ext2_dir_add_entry(EXT2_ROOT_INO, "old.txt", ino, EXT2_FT_REG_FILE);

    /* Verify old exists, new doesn't. */
    ASSERT_NEQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "old.txt"), 0);
    ASSERT_EQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "new.txt"), 0);

    /* Rename. */
    ext2_status_t st = ext2_rename(EXT2_ROOT_INO, "old.txt",
                                    EXT2_ROOT_INO, "new.txt");
    ASSERT_EQ(dev, (int)st, (int)EXT2_OK);

    /* Old gone, new present. */
    ASSERT_EQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "old.txt"), 0);
    ASSERT_NEQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "new.txt"), 0);

    ext2_unmount();
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ext2_phase9(void) {
    mock_register_once();

    test_register("ext2_stat_new",   test_stat_new_file);
    test_register("ext2_stat_dir",   test_stat_dir);
    test_register("ext2_lseek_set",  test_lseek_set);
    test_register("ext2_lseek_cur",  test_lseek_cur);
    test_register("ext2_lseek_end",  test_lseek_end);
    test_register("ext2_unlink_f",   test_unlink_file);
    test_register("ext2_rmdir_emp",  test_rmdir_empty);
    test_register("ext2_rmdir_full", test_rmdir_nonempty);
    test_register("ext2_getdents",   test_getdents_dir);
    test_register("ext2_rename_sd",  test_rename_same_dir);
}
