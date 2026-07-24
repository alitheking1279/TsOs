/**
 * @file test_vfs.c
 * @brief VFS (Virtual Filesystem Switch) tests.
 *
 * Tests:
 *   Group R — VFS Operations (init, mount, resolve, unmount, open, stat, etc.)
 */

#include "test.h"
#include "../fs/vfs.h"
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
    mock_storage[8 * MOCK_SECTOR_SIZE]     = 0xFF;
}

static void vfs_test_setup(void) {
    mock_reset();
    mock_format_ext2();
    mock_register_once();
    bcache_invalidate_all();
    ext2_init(NULL);
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();
}

/* =========================================================================
 * Group R — VFS Operations
 * ========================================================================= */

static void test_vfs_init(serial_dev_t *dev) {
    vfs_init();
    /* After init, resolve_path should fail for any path. */
    const char *rel = NULL;
    vfs_mount_entry_t *mnt = NULL;
    int r = vfs_resolve_path("/", &rel, &mnt);
    ASSERT_EQ(dev, r, -1);
}

static void test_vfs_mount_root(serial_dev_t *dev) {
    vfs_init();
    vfs_register_fs(&ext2_vfs_ops);

    vfs_test_setup();

    int r = vfs_mount("/", &ext2_vfs_ops, ext2_get_fs());
    ASSERT_EQ(dev, r, 0);

    vfs_unmount("/");
}

static void test_vfs_resolve_root(serial_dev_t *dev) {
    vfs_init();
    vfs_register_fs(&ext2_vfs_ops);
    vfs_test_setup();
    vfs_mount("/", &ext2_vfs_ops, ext2_get_fs());

    const char *rel = NULL;
    vfs_mount_entry_t *mnt = NULL;

    /* "/" should resolve. */
    int r = vfs_resolve_path("/", &rel, &mnt);
    ASSERT_EQ(dev, r, 0);
    ASSERT_NEQ(dev, (int)mnt, 0);

    /* "/foo" should also resolve to root mount. */
    r = vfs_resolve_path("/foo", &rel, &mnt);
    ASSERT_EQ(dev, r, 0);

    vfs_unmount("/");
}

static void test_vfs_unmount(serial_dev_t *dev) {
    vfs_init();
    vfs_register_fs(&ext2_vfs_ops);
    vfs_test_setup();
    vfs_mount("/", &ext2_vfs_ops, ext2_get_fs());

    int r = vfs_unmount("/");
    ASSERT_EQ(dev, r, 0);

    /* After unmount, resolve should fail. */
    const char *rel = NULL;
    vfs_mount_entry_t *mnt = NULL;
    r = vfs_resolve_path("/", &rel, &mnt);
    ASSERT_EQ(dev, r, -1);
}

static void test_vfs_double_mount(serial_dev_t *dev) {
    vfs_init();
    vfs_register_fs(&ext2_vfs_ops);
    vfs_test_setup();

    int r1 = vfs_mount("/", &ext2_vfs_ops, ext2_get_fs());
    ASSERT_EQ(dev, r1, 0);

    /* Second mount at "/" should fail. */
    int r2 = vfs_mount("/", &ext2_vfs_ops, ext2_get_fs());
    ASSERT_EQ(dev, r2, -1);

    vfs_unmount("/");
}

static void test_vfs_open_close(serial_dev_t *dev) {
    vfs_init();
    vfs_register_fs(&ext2_vfs_ops);
    vfs_test_setup();
    vfs_mount("/", &ext2_vfs_ops, ext2_get_fs());

    /* Open a new file with O_CREAT. */
    int fd = vfs_open("/test.txt", O_CREAT | O_RDWR);
    ASSERT_TRUE(dev, fd >= 0);

    /* Close it. */
    task_t *cur = task_get_current();
    if (cur->fd_table[fd].ops->close)
        cur->fd_table[fd].ops->close(fd, cur->fd_table[fd].data);
    memset(&cur->fd_table[fd], 0, sizeof(file_t));

    vfs_unmount("/");
}

static void test_vfs_stat_file(serial_dev_t *dev) {
    vfs_init();
    vfs_register_fs(&ext2_vfs_ops);
    vfs_test_setup();
    vfs_mount("/", &ext2_vfs_ops, ext2_get_fs());

    /* Create a file via ext2 directly (write content). */
    int32_t ino = ext2_alloc_inode();
    ASSERT_TRUE(dev, ino > 0);
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT2_S_IFREG | 0644;
    inode.i_links_count = 1;
    ext2_write_inode((uint32_t)ino, &inode);
    ext2_dir_add_entry(EXT2_ROOT_INO, "stat.txt", (uint32_t)ino, EXT2_FT_REG_FILE);

    /* Stat it through VFS. */
    ext2_stat_t st;
    int r = vfs_stat("/stat.txt", &st);
    ASSERT_EQ(dev, r, 0);
    ASSERT_EQ(dev, (int)st.ino, (int)ino);
    ASSERT_EQ(dev, (int)(st.mode & 0xF000), (int)EXT2_S_IFREG);

    vfs_unmount("/");
}

static void test_vfs_unlink_file(serial_dev_t *dev) {
    vfs_init();
    vfs_register_fs(&ext2_vfs_ops);
    vfs_test_setup();
    vfs_mount("/", &ext2_vfs_ops, ext2_get_fs());

    /* Create a file. */
    int32_t ino = ext2_alloc_inode();
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT2_S_IFREG | 0644;
    inode.i_links_count = 1;
    ext2_write_inode((uint32_t)ino, &inode);
    ext2_dir_add_entry(EXT2_ROOT_INO, "del.txt", (uint32_t)ino, EXT2_FT_REG_FILE);

    /* Verify it exists. */
    ASSERT_NEQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "del.txt"), 0);

    /* Unlink through VFS. */
    int r = vfs_unlink("/del.txt");
    ASSERT_EQ(dev, r, 0);

    /* Verify it's gone. */
    ASSERT_EQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "del.txt"), 0);

    vfs_unmount("/");
}

static void test_vfs_mkdir_rmdir(serial_dev_t *dev) {
    vfs_init();
    vfs_register_fs(&ext2_vfs_ops);
    vfs_test_setup();
    vfs_mount("/", &ext2_vfs_ops, ext2_get_fs());

    /* Create a directory through VFS. */
    int r = vfs_mkdir("/newdir", 0755);
    ASSERT_EQ(dev, r, 0);

    /* Verify it exists. */
    ASSERT_NEQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "newdir"), 0);

    /* Remove it through VFS. */
    r = vfs_rmdir("/newdir");
    ASSERT_EQ(dev, r, 0);

    /* Verify it's gone. */
    ASSERT_EQ(dev, (int)ext2_dir_lookup(EXT2_ROOT_INO, "newdir"), 0);

    vfs_unmount("/");
}

static void test_vfs_no_mount_open(serial_dev_t *dev) {
    vfs_init();
    /* No mount — open should fail. */
    int fd = vfs_open("/foo", 0);
    ASSERT_EQ(dev, fd, -1);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_vfs(void) {
    mock_register_once();

    test_register("vfs_init",       test_vfs_init);
    test_register("vfs_mount_rt",   test_vfs_mount_root);
    test_register("vfs_res_root",   test_vfs_resolve_root);
    test_register("vfs_unmount",    test_vfs_unmount);
    test_register("vfs_dbl_mount",  test_vfs_double_mount);
    test_register("vfs_open_cl",    test_vfs_open_close);
    test_register("vfs_stat_f",     test_vfs_stat_file);
    test_register("vfs_unlink_f",   test_vfs_unlink_file);
    test_register("vfs_mkdir_rm",   test_vfs_mkdir_rmdir);
    test_register("vfs_no_mount",   test_vfs_no_mount_open);
}
