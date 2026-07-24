/**
 * @file test_ext2_syscall.c
 * @brief ext2 file descriptor integration tests.
 *
 * Tests the ext2_file_ops vtable (read/write/close) and fd integration
 * without going through the actual syscall handler (which needs a task).
 *
 * Tests:
 *   Group P — File Descriptor Integration
 */

#include "test.h"
#include "../fs/ext2.h"
#include "../fs/bcache.h"
#include "../fs/block_dev.h"
#include "../kernel/kheap.h"
#include "../kernel/task.h"
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
 * Helper: create a file via path resolution, similar to what sys_open does.
 * Returns the inode number of the new file, or 0 on error.
 */
static uint32_t create_file_via_path(const char *dir_path, const char *name) {
    uint32_t dir_ino = ext2_resolve_path(dir_path);
    if (dir_ino == 0) return 0;

    int32_t file_ino = ext2_alloc_inode();
    if (file_ino < 0) return 0;

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = EXT2_S_IFREG | 0644;
    inode.i_links_count = 1;
    ext2_write_inode((uint32_t)file_ino, &inode);

    ext2_status_t st = ext2_dir_add_entry(dir_ino, name,
                                           (uint32_t)file_ino,
                                           EXT2_FT_REG_FILE);
    if (st != EXT2_OK) return 0;
    return (uint32_t)file_ino;
}

/* =========================================================================
 * Group P — File Descriptor Integration
 * ========================================================================= */

static void test_fd_open_close(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    /* Simulate sys_open: resolve path, allocate ext2_file_t, set up fd entry. */
    uint32_t ino = create_file_via_path("/", "hello.txt");
    ASSERT_NEQ(dev, (int)ino, 0);

    ext2_file_t *file = (ext2_file_t *)kmalloc(sizeof(ext2_file_t));
    ASSERT_NEQ(dev, (int)file, 0);
    file->ino    = ino;
    file->offset = 0;
    file->flags  = O_RDONLY;

    /* Simulate sys_close: call ops->close, free file. */
    ext2_file_ops.close(0, file);

    ext2_unmount();
}

static void test_fd_open_nonexist(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    /* Path resolution for non-existent file returns 0. */
    uint32_t ino = ext2_resolve_path("/nonexistent.txt");
    ASSERT_EQ(dev, (int)ino, 0);

    ext2_unmount();
}

static void test_fd_write_read(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t ino = create_file_via_path("/", "data.bin");
    ASSERT_NEQ(dev, (int)ino, 0);

    /* Open for write. */
    ext2_file_t *wf = (ext2_file_t *)kmalloc(sizeof(ext2_file_t));
    wf->ino    = ino;
    wf->offset = 0;
    wf->flags  = O_WRONLY;

    const char *msg = "Hello ext2!";
    int64_t written = ext2_file_ops.write(0, msg, 11, wf);
    ASSERT_EQ(dev, (int)written, 11);
    ASSERT_EQ(dev, (int)wf->offset, 11);
    ext2_file_ops.close(0, wf);

    /* Open for read. */
    ext2_file_t *rf = (ext2_file_t *)kmalloc(sizeof(ext2_file_t));
    rf->ino    = ino;
    rf->offset = 0;
    rf->flags  = O_RDONLY;

    char buf[32];
    memset(buf, 0, sizeof(buf));
    int64_t nread = ext2_file_ops.read(0, buf, 11, rf);
    ASSERT_EQ(dev, (int)nread, 11);
    ASSERT_EQ(dev, (int)rf->offset, 11);
    ASSERT_EQ(dev, (int)strcmp(buf, "Hello ext2!"), 0);
    ext2_file_ops.close(0, rf);

    ext2_unmount();
}

static void test_fd_offset(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t ino = create_file_via_path("/", "offset.bin");
    ASSERT_NEQ(dev, (int)ino, 0);

    /* Write 100 bytes. */
    ext2_file_t *wf = (ext2_file_t *)kmalloc(sizeof(ext2_file_t));
    wf->ino    = ino;
    wf->offset = 0;
    wf->flags  = O_WRONLY;

    uint8_t wdata[100];
    memset(wdata, 0xAA, 100);
    ext2_file_ops.write(0, wdata, 100, wf);
    ASSERT_EQ(dev, (int)wf->offset, 100);
    ext2_file_ops.close(0, wf);

    /* Reopen for read, read 50 bytes. */
    ext2_file_t *rf = (ext2_file_t *)kmalloc(sizeof(ext2_file_t));
    rf->ino    = ino;
    rf->offset = 0;
    rf->flags  = O_RDONLY;

    uint8_t rbuf[50];
    ext2_file_ops.read(0, rbuf, 50, rf);
    ASSERT_EQ(dev, (int)rf->offset, 50);

    /* Read 50 more — offset should be 100. */
    ext2_file_ops.read(0, rbuf, 50, rf);
    ASSERT_EQ(dev, (int)rf->offset, 100);

    ext2_file_ops.close(0, rf);
    ext2_unmount();
}

static void test_fd_close_invalid(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    /* close(NULL) should not crash — kfree handles NULL. */
    ext2_file_ops.close(-1, NULL);

    /* close with bad pointer should not crash (kfree checks range). */
    ext2_file_ops.close(99, (void *)0xDEAD);

    ext2_unmount();
}

static void test_fd_two_fds(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);
    ext2_init_root_dir();

    uint32_t ino = create_file_via_path("/", "dual.bin");
    ASSERT_NEQ(dev, (int)ino, 0);

    /* Two independent file handles on the same inode. */
    ext2_file_t *f1 = (ext2_file_t *)kmalloc(sizeof(ext2_file_t));
    f1->ino = ino; f1->offset = 0; f1->flags = O_WRONLY;

    ext2_file_t *f2 = (ext2_file_t *)kmalloc(sizeof(ext2_file_t));
    f2->ino = ino; f2->offset = 0; f2->flags = O_RDONLY;

    /* Write 50 bytes via f1. */
    uint8_t wdata[50];
    memset(wdata, 0xBB, 50);
    ext2_file_ops.write(0, wdata, 50, f1);
    ASSERT_EQ(dev, (int)f1->offset, 50);

    /* f2 offset is independent — still 0. */
    ASSERT_EQ(dev, (int)f2->offset, 0);

    /* Read via f2. */
    uint8_t rbuf[50];
    ext2_file_ops.read(0, rbuf, 50, f2);
    ASSERT_EQ(dev, (int)f2->offset, 50);
    for (int i = 0; i < 50; i++) {
        ASSERT_EQ(dev, rbuf[i], 0xBB);
    }

    ext2_file_ops.close(0, f1);
    ext2_file_ops.close(0, f2);
    ext2_unmount();
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ext2_syscall(void) {
    mock_register_once();

    test_register("ext2_fd_openclose", test_fd_open_close);
    test_register("ext2_fd_nonexist", test_fd_open_nonexist);
    test_register("ext2_fd_wrdwr",    test_fd_write_read);
    test_register("ext2_fd_offset",   test_fd_offset);
    test_register("ext2_fd_badclose", test_fd_close_invalid);
    test_register("ext2_fd_two",      test_fd_two_fds);
}
