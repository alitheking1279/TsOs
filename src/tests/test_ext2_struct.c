/**
 * @file test_ext2_struct.c
 * @brief ext2 filesystem structure and mount test suite.
 *
 * Tests:
 *   Group A — On-disk structure sizes and field offsets
 *   Group B — Superblock field offset validation
 *   Group C — Group descriptor field offset validation
 *   Group D — Inode field offset validation
 *   Group E — Directory entry field offset validation
 *   Group F — Block size and geometry macros
 *   Group G — Mount / unmount lifecycle (mock device)
 *   Group H — Block read/write through bcache (mock device)
 *   Group I — Root directory initialization (mock device)
 *
 * NOTE: We register ONE mock block device and reuse it across all tests.
 * The pool is only 8 slots; previous test suites consume most of them.
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

/**
 * @brief Register the mock device exactly once. Returns the device ID.
 *
 * Call this from the registration function. The returned ID is stored
 * in g_mock_dev_id and reused by all tests.
 */
static uint8_t g_mock_dev_id = 0xFF;

static void mock_register_once(void) {
    /* Always unregister first to ensure we own the slot. */
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
 * @brief Format mock_storage as a valid ext2 image.
 *
 * Layout: 1K blocks, 4096 inodes, 1 group.
 * Block 1 = superblock, Block 2 = group descriptor,
 * Block 3 = block bitmap, Block 4 = inode bitmap,
 * Blocks 5-12 = inode table, Blocks 13+ = data.
 */
static void mock_format_ext2(void) {
    memset(mock_storage, 0, sizeof(mock_storage));

    /* --- Superblock at block 1 (sector 2-3) --- */
    ext2_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count     = 4096;
    sb.s_blocks_count     = 256;
    sb.s_r_blocks_count   = 0;
    sb.s_free_blocks_count = 243;
    sb.s_free_inodes_count = 4094;
    sb.s_first_data_block = 1;
    sb.s_log_block_size   = 0;       /* 1024 */
    sb.s_log_frag_size    = 0;
    sb.s_blocks_per_group = 256;
    sb.s_frags_per_group  = 256;
    sb.s_inodes_per_group = 4096;
    sb.s_mtime            = 0;
    sb.s_wtime            = 0;
    sb.s_mnt_count        = 1;
    sb.s_max_mnt_count    = -1;
    sb.s_magic            = EXT2_SUPER_MAGIC;
    sb.s_state            = EXT2_FS_CLEAN;
    sb.s_errors           = EXT2_ERRORS_CONTINUE;
    sb.s_minor_rev_level  = 0;
    sb.s_lastcheck        = 0;
    sb.s_checkinterval    = 0;
    sb.s_creator_os       = 0;
    sb.s_rev_level        = 1;
    sb.s_first_ino        = 11;
    sb.s_inode_size       = 128;
    sb.s_block_group_nr   = 0;
    sb.s_feature_compat   = 0;
    sb.s_feature_incompat = 0;
    sb.s_feature_ro_compat = 0;
    memset(sb.s_uuid, 0xAB, 16);
    memcpy(sb.s_volume_name, "test", 5);
    memcpy(sb.s_last_mounted, "/mnt", 5);

    memcpy(&mock_storage[2 * MOCK_SECTOR_SIZE], &sb, sizeof(sb));

    /* --- Group descriptor at block 2 (sectors 4-5) --- */
    ext2_group_desc_t gd;
    memset(&gd, 0, sizeof(gd));
    gd.bg_block_bitmap     = 3;
    gd.bg_inode_bitmap     = 4;
    gd.bg_inode_table      = 5;
    gd.bg_free_blocks_count = 243;
    gd.bg_free_inodes_count = 4094;
    gd.bg_used_dirs_count  = 1;

    memcpy(&mock_storage[4 * MOCK_SECTOR_SIZE], &gd, sizeof(gd));

    /* Mark blocks 0-12 as used in the block bitmap (block 3, sector 6) */
    mock_storage[6 * MOCK_SECTOR_SIZE] = 0xFF;     /* bits 0-7 */
    mock_storage[6 * MOCK_SECTOR_SIZE + 1] = 0x1F; /* bits 8-12 */

    /* Mark inodes 1-8 as used in the inode bitmap (block 4, sector 8) */
    mock_storage[8 * MOCK_SECTOR_SIZE] = 0xFF;
}

/**
 * @brief Setup helper: reset storage, format ext2, ensure device registered.
 */
static void ext2_test_setup(void) {
    mock_reset();
    mock_format_ext2();
    mock_register_once();
    bcache_invalidate_all();
    ext2_init(NULL);
}

/* =========================================================================
 * Group A — On-disk structure sizes
 * ========================================================================= */

static void test_ext2_superblock_size(serial_dev_t *dev) {
    ASSERT_EQ(dev, sizeof(ext2_superblock_t), 1024);
}

static void test_ext2_group_desc_size(serial_dev_t *dev) {
    ASSERT_EQ(dev, sizeof(ext2_group_desc_t), 32);
}

static void test_ext2_inode_size(serial_dev_t *dev) {
    ASSERT_EQ(dev, sizeof(ext2_inode_t), 128);
}

static void test_ext2_dirent_size(serial_dev_t *dev) {
    ASSERT_EQ(dev, sizeof(ext2_dirent_t), 8);
}

/* =========================================================================
 * Group B — Superblock field offsets
 * ========================================================================= */

static void test_sb_inodes_count(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_inodes_count), 0);
}

static void test_sb_blocks_count(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_blocks_count), 4);
}

static void test_sb_first_data_block(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_first_data_block), 20);
}

static void test_sb_log_block_size(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_log_block_size), 24);
}

static void test_sb_magic(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_magic), 56);
}

static void test_sb_state(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_state), 58);
}

static void test_sb_rev_level(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_rev_level), 76);
}

static void test_sb_first_ino(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_first_ino), 84);
}

static void test_sb_inode_size(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_inode_size), 88);
}

static void test_sb_uuid(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_uuid), 104);
}

static void test_sb_volume_name(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_volume_name), 120);
}

static void test_sb_reserved(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_reserved), 264);
}

static void test_sb_two(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_two), 508);
}

static void test_sb_minor(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_minor), 510);
}

static void test_sb_major(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_major), 511);
}

static void test_sb_reserved2(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_superblock_t, s_reserved2), 512);
}

/* =========================================================================
 * Group C — Group descriptor field offsets
 * ========================================================================= */

static void test_gd_block_bitmap(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_group_desc_t, bg_block_bitmap), 0);
}

static void test_gd_inode_bitmap(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_group_desc_t, bg_inode_bitmap), 4);
}

static void test_gd_inode_table(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_group_desc_t, bg_inode_table), 8);
}

static void test_gd_free_blocks(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_group_desc_t, bg_free_blocks_count), 12);
}

static void test_gd_free_inodes(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_group_desc_t, bg_free_inodes_count), 14);
}

static void test_gd_used_dirs(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_group_desc_t, bg_used_dirs_count), 16);
}

static void test_gd_pad(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_group_desc_t, bg_pad), 18);
}

/* =========================================================================
 * Group D — Inode field offsets
 * ========================================================================= */

static void test_ino_mode(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_mode), 0);
}

static void test_ino_uid(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_uid), 2);
}

static void test_ino_size(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_size), 4);
}

static void test_ino_blocks(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_blocks), 28);
}

static void test_ino_flags(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_flags), 32);
}

static void test_ino_block(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_block), 40);
}

static void test_ino_generation(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_generation), 100);
}

static void test_ino_file_acl(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_file_acl), 104);
}

static void test_ino_size_high(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_size_high), 108);
}

static void test_ino_fsize(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_fsize), 112);
}

static void test_ino_osd2(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_inode_t, i_osd2), 116);
}

/* =========================================================================
 * Group E — Directory entry field offsets
 * ========================================================================= */

static void test_dirent_inode(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_dirent_t, inode), 0);
}

static void test_dirent_rec_len(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_dirent_t, rec_len), 4);
}

static void test_dirent_name_len(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_dirent_t, name_len), 6);
}

static void test_dirent_file_type(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_dirent_t, file_type), 7);
}

static void test_dirent_name(serial_dev_t *dev) {
    ASSERT_EQ(dev, offsetof(ext2_dirent_t, name), 8);
}

/* =========================================================================
 * Group F — Constants and macros
 * ========================================================================= */

static void test_magic_value(serial_dev_t *dev) {
    ASSERT_EQ(dev, EXT2_SUPER_MAGIC, 0xEF53);
}

static void test_root_ino(serial_dev_t *dev) {
    ASSERT_EQ(dev, (int)EXT2_ROOT_INO, 2);
}

static void test_n_direct(serial_dev_t *dev) {
    ASSERT_EQ(dev, (int)EXT2_N_DIRECT, 12);
}

static void test_n_blocks(serial_dev_t *dev) {
    ASSERT_EQ(dev, (int)EXT2_N_BLOCKS, 15);
}

static void test_descs_per_block_1k(serial_dev_t *dev) {
    ASSERT_EQ(dev, (int)EXT2_DESCS_PER_BLOCK(1024), 32);
}

static void test_descs_per_block_4k(serial_dev_t *dev) {
    ASSERT_EQ(dev, (int)EXT2_DESCS_PER_BLOCK(4096), 128);
}

static void test_block_size_log0(serial_dev_t *dev) {
    ext2_superblock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.s_log_block_size = 0;
    ASSERT_EQ(dev, (int)EXT2_BLOCK_SIZE(&tmp), 1024);
}

static void test_block_size_log2(serial_dev_t *dev) {
    ext2_superblock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.s_log_block_size = 2;
    ASSERT_EQ(dev, (int)EXT2_BLOCK_SIZE(&tmp), 4096);
}

static void test_num_groups(serial_dev_t *dev) {
    ext2_superblock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.s_blocks_count = 4096;
    tmp.s_blocks_per_group = 1024;
    ASSERT_EQ(dev, (int)EXT2_NUM_GROUPS(&tmp), 4);
}

static void test_ino_group(serial_dev_t *dev) {
    ASSERT_EQ(dev, (int)EXT2_INO_GROUP(1, 4096), 0);
    ASSERT_EQ(dev, (int)EXT2_INO_GROUP(4097, 4096), 1);
}

/* =========================================================================
 * Group G — Mount / unmount lifecycle
 * ========================================================================= */

static void test_mount_unmount(serial_dev_t *dev) {
    ext2_test_setup();

    ASSERT_EQ(dev, ext2_is_mounted(), false);
    ASSERT_NULL(dev, ext2_get_fs());

    ext2_status_t st = ext2_mount(g_mock_dev_id);
    ASSERT_EQ(dev, (int)st, EXT2_OK);
    ASSERT_EQ(dev, ext2_is_mounted(), true);

    ext2_fs_t *fs = ext2_get_fs();
    ASSERT_NOT_NULL(dev, fs);
    ASSERT_EQ(dev, fs->mounted, true);
    ASSERT_EQ(dev, (int)fs->block_size, 1024);
    ASSERT_EQ(dev, (int)fs->num_groups, 1);
    ASSERT_EQ(dev, (int)fs->inodes_per_group, 4096);
    ASSERT_EQ(dev, (int)fs->blocks_per_group, 256);
    ASSERT_EQ(dev, fs->gd != NULL, true);

    ext2_unmount();
    ASSERT_EQ(dev, ext2_is_mounted(), false);
    ASSERT_NULL(dev, ext2_get_fs());
}

static void test_mount_bad_magic(serial_dev_t *dev) {
    ext2_test_setup();

    /* Overwrite magic with zeros. */
    uint8_t bad[2] = {0x00, 0x00};
    memcpy(&mock_storage[2 * MOCK_SECTOR_SIZE + 56], bad, 2);

    ext2_status_t st = ext2_mount(g_mock_dev_id);
    ASSERT_EQ(dev, (int)st, EXT2_ERR_BAD_MAGIC);
    ASSERT_EQ(dev, ext2_is_mounted(), false);
}

static void test_mount_already_mounted(serial_dev_t *dev) {
    ext2_test_setup();

    ext2_status_t st1 = ext2_mount(g_mock_dev_id);
    ASSERT_EQ(dev, (int)st1, EXT2_OK);

    /* Second mount should fail. */
    ext2_status_t st2 = ext2_mount(g_mock_dev_id);
    ASSERT_EQ(dev, (int)st2, EXT2_ERR_INVALID);

    ext2_unmount();
}

static void test_read_block_not_mounted(serial_dev_t *dev) {
    ext2_init(NULL);
    uint8_t buf[1024];
    ext2_status_t st = ext2_read_block(0, buf);
    ASSERT_EQ(dev, (int)st, EXT2_ERR_NOT_MOUNTED);
}

static void test_write_block_not_mounted(serial_dev_t *dev) {
    ext2_init(NULL);
    uint8_t buf[1024];
    ext2_status_t st = ext2_write_block(0, buf);
    ASSERT_EQ(dev, (int)st, EXT2_ERR_NOT_MOUNTED);
}

/* =========================================================================
 * Group H — Block read/write through bcache
 * ========================================================================= */

static void test_read_block_0(serial_dev_t *dev) {
    ext2_test_setup();

    /* Put a known pattern in block 0 (boot block, sectors 0-1). */
    memset(&mock_storage[0], 0x42, MOCK_EXT2_BLOCK);

    ext2_mount(g_mock_dev_id);

    uint8_t buf[1024];
    ext2_status_t st = ext2_read_block(0, buf);
    ASSERT_EQ(dev, (int)st, EXT2_OK);
    ASSERT_EQ(dev, buf[0], 0x42);
    ASSERT_EQ(dev, buf[1023], 0x42);

    ext2_unmount();
}

static void test_write_block_roundtrip(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    uint8_t wbuf[1024];
    memset(wbuf, 0xAA, 1024);
    wbuf[0] = 0xBB;
    wbuf[1023] = 0xCC;

    ext2_status_t st = ext2_write_block(20, wbuf);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    uint8_t rbuf[1024];
    st = ext2_read_block(20, rbuf);
    ASSERT_EQ(dev, (int)st, EXT2_OK);
    ASSERT_EQ(dev, rbuf[0], 0xBB);
    ASSERT_EQ(dev, rbuf[1023], 0xCC);

    ext2_unmount();
}

static void test_read_superblock(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    uint8_t buf[1024];
    ext2_status_t st = ext2_read_block(1, buf);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    ext2_superblock_t *sb = (ext2_superblock_t *)buf;
    ASSERT_EQ(dev, sb->s_magic, EXT2_SUPER_MAGIC);
    ASSERT_EQ(dev, (int)sb->s_log_block_size, 0);
    ASSERT_EQ(dev, sb->s_inodes_count, 4096);

    ext2_unmount();
}

static void test_write_read_superblock_updates(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_fs_t *fs = ext2_get_fs();
    fs->sb.s_mnt_count = 5;

    ext2_status_t st = ext2_write_block(1, &fs->sb);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    uint8_t buf[1024];
    st = ext2_read_block(1, buf);
    ASSERT_EQ(dev, (int)st, EXT2_OK);
    ext2_superblock_t *disk_sb = (ext2_superblock_t *)buf;
    ASSERT_EQ(dev, disk_sb->s_mnt_count, 5);

    ext2_unmount();
}

/* =========================================================================
 * Group I — Root directory initialization
 * ========================================================================= */

static void test_init_root_dir(serial_dev_t *dev) {
    ext2_test_setup();
    ext2_mount(g_mock_dev_id);

    ext2_status_t st = ext2_init_root_dir();
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    /* Read back root inode (inode 2 at byte 128 in inode table block 5). */
    uint8_t itable_buf[1024];
    st = ext2_read_block(5, itable_buf);
    ASSERT_EQ(dev, (int)st, EXT2_OK);

    ext2_inode_t *root = (ext2_inode_t *)(itable_buf + 128);
    ASSERT_EQ(dev, root->i_mode & 0xF000, EXT2_S_IFDIR);
    ASSERT_EQ(dev, root->i_links_count, 2);
    ASSERT_EQ(dev, root->i_block[0], 13);

    /* Read root data block (block 13) and verify "." and ".." entries. */
    uint8_t dir_buf[1024];
    st = ext2_read_block(13, dir_buf);
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

static void test_init_root_dir_not_mounted(serial_dev_t *dev) {
    ext2_init(NULL);
    ext2_status_t st = ext2_init_root_dir();
    ASSERT_EQ(dev, (int)st, EXT2_ERR_NOT_MOUNTED);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_ext2_struct(void) {
    /* Register the shared mock device once. */
    mock_register_once();

    /* Group A — sizes */
    test_register("ext2_sb_size",          test_ext2_superblock_size);
    test_register("ext2_gd_size",          test_ext2_group_desc_size);
    test_register("ext2_ino_size",         test_ext2_inode_size);
    test_register("ext2_dirent_size",      test_ext2_dirent_size);

    /* Group B — superblock offsets */
    test_register("ext2_sb_off_inodes",    test_sb_inodes_count);
    test_register("ext2_sb_off_blocks",    test_sb_blocks_count);
    test_register("ext2_sb_off_first_blk", test_sb_first_data_block);
    test_register("ext2_sb_off_log_blk",   test_sb_log_block_size);
    test_register("ext2_sb_off_magic",     test_sb_magic);
    test_register("ext2_sb_off_state",     test_sb_state);
    test_register("ext2_sb_off_rev",       test_sb_rev_level);
    test_register("ext2_sb_off_first_ino", test_sb_first_ino);
    test_register("ext2_sb_off_ino_size",  test_sb_inode_size);
    test_register("ext2_sb_off_uuid",      test_sb_uuid);
    test_register("ext2_sb_off_vol_name",  test_sb_volume_name);
    test_register("ext2_sb_off_reserved",  test_sb_reserved);
    test_register("ext2_sb_off_two",       test_sb_two);
    test_register("ext2_sb_off_minor",     test_sb_minor);
    test_register("ext2_sb_off_major",     test_sb_major);
    test_register("ext2_sb_off_reserved2", test_sb_reserved2);

    /* Group C — group descriptor offsets */
    test_register("ext2_gd_off_blk_bm",   test_gd_block_bitmap);
    test_register("ext2_gd_off_ino_bm",   test_gd_inode_bitmap);
    test_register("ext2_gd_off_ino_tbl",  test_gd_inode_table);
    test_register("ext2_gd_off_free_blk", test_gd_free_blocks);
    test_register("ext2_gd_off_free_ino", test_gd_free_inodes);
    test_register("ext2_gd_off_used_dir", test_gd_used_dirs);
    test_register("ext2_gd_off_pad",      test_gd_pad);

    /* Group D — inode offsets */
    test_register("ext2_ino_off_mode",     test_ino_mode);
    test_register("ext2_ino_off_uid",      test_ino_uid);
    test_register("ext2_ino_off_size",     test_ino_size);
    test_register("ext2_ino_off_blocks",   test_ino_blocks);
    test_register("ext2_ino_off_flags",    test_ino_flags);
    test_register("ext2_ino_off_block",    test_ino_block);
    test_register("ext2_ino_off_gen",      test_ino_generation);
    test_register("ext2_ino_off_acl",      test_ino_file_acl);
    test_register("ext2_ino_off_size_hi",  test_ino_size_high);
    test_register("ext2_ino_off_fsize",    test_ino_fsize);
    test_register("ext2_ino_off_osd2",     test_ino_osd2);

    /* Group E — directory entry offsets */
    test_register("ext2_de_off_inode",     test_dirent_inode);
    test_register("ext2_de_off_rec_len",   test_dirent_rec_len);
    test_register("ext2_de_off_name_len",  test_dirent_name_len);
    test_register("ext2_de_off_file_type", test_dirent_file_type);
    test_register("ext2_de_off_name",      test_dirent_name);

    /* Group F — constants and macros */
    test_register("ext2_const_magic",      test_magic_value);
    test_register("ext2_const_root_ino",   test_root_ino);
    test_register("ext2_const_n_direct",   test_n_direct);
    test_register("ext2_const_n_blocks",   test_n_blocks);
    test_register("ext2_dpb_1k",           test_descs_per_block_1k);
    test_register("ext2_dpb_4k",           test_descs_per_block_4k);
    test_register("ext2_bsize_log0",       test_block_size_log0);
    test_register("ext2_bsize_log2",       test_block_size_log2);
    test_register("ext2_num_groups",       test_num_groups);
    test_register("ext2_ino_group",        test_ino_group);

    /* Group G — mount/unmount lifecycle */
    test_register("ext2_mount_unmount",    test_mount_unmount);
    test_register("ext2_mount_bad_magic",  test_mount_bad_magic);
    test_register("ext2_mount_already",    test_mount_already_mounted);
    test_register("ext2_rd_not_mounted",   test_read_block_not_mounted);
    test_register("ext2_wr_not_mounted",   test_write_block_not_mounted);

    /* Group H — block I/O */
    test_register("ext2_read_block_0",     test_read_block_0);
    test_register("ext2_wr_roundtrip",     test_write_block_roundtrip);
    test_register("ext2_read_superblock",  test_read_superblock);
    test_register("ext2_sb_update",        test_write_read_superblock_updates);

    /* Group I — root directory */
    test_register("ext2_init_root",        test_init_root_dir);
    test_register("ext2_init_root_nomnt",  test_init_root_dir_not_mounted);
}
