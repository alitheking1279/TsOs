/**
 * @file ext2.h
 * @brief ext2 filesystem — on-disk structures, constants, and in-memory state.
 *
 * Defines the ext2 on-disk layout per the ext2 specification:
 *   - Superblock (1024 bytes, at byte offset 1024)
 *   - Block Group Descriptor (32 bytes)
 *   - Inode (128 bytes)
 *   - Directory Entry (variable length, 4-byte aligned)
 *
 * Target configuration: 1024-byte blocks on a 4 MiB disk image.
 *   s_log_block_size = 0  →  block_size = 1024 << 0 = 1024
 *   s_first_data_block = 1  (since block_size == 1024)
 *
 * Block layout for 4 MiB (4096 blocks @ 1K):
 *   Block 0:  Boot sector (unused)
 *   Block 1:  Superblock (bytes 1024-2047)
 *   Block 2:  Block Group Descriptors
 *   Block 3:  Block Bitmap
 *   Block 4:  Inode Bitmap
 *   Block 5:  Inode Table (blocks 5-12, 1024 inodes @ 128B)
 *   Block 13+: Data blocks
 *
 * ext2 is little-endian; x86-64 is little-endian, so on-disk structs
 * can be read/written directly via memcpy without byte-swapping.
 *
 * References:
 *   ext2 spec: https://www.nongnu.org/ext2-internals/ext2-overview.html
 *   Linux kernel: fs/ext2/ext2.h
 */

#ifndef FS_EXT2_H
#define FS_EXT2_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../kernel/task.h"

/* Forward declaration for VFS layer. */
struct vfs_fs_ops;
typedef struct vfs_fs_ops vfs_fs_ops_t;

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** ext2 superblock magic number (offset 56 in the superblock). */
#define EXT2_SUPER_MAGIC        0xEF53

/** Root directory inode number (always 2 in ext2). */
#define EXT2_ROOT_INO           2

/** Inode size in bytes (rev 0: always 128). */
#define EXT2_INODE_SIZE         128

/** Maximum number of direct block pointers in an inode. */
#define EXT2_N_BLOCKS           15

/** Number of direct block pointers (before indirect). */
#define EXT2_N_DIRECT           12

/* =========================================================================
 * Superblock
 *
 * Located at byte offset 1024 on disk (block 1 for 1K block size).
 * Primary magic at offset 56; secondary magic (0xEF53) at offset 508.
 * Total size: 1024 bytes (padded to end).
 * ========================================================================= */

/**
 * @brief ext2 superblock — the filesystem metadata root.
 *
 * Every field is stored little-endian. x86-64 reads these directly.
 * Fields marked (ext2 rev >= 1) are only valid when s_rev_level >= 1.
 */
typedef struct {
    /* --- Base superblock (rev 0) --- */
    uint32_t s_inodes_count;            /**< +0   Total inode count. */
    uint32_t s_blocks_count;            /**< +4   Total block count. */
    uint32_t s_r_blocks_count;          /**< +8   Reserved (root) block count. */
    uint32_t s_free_blocks_count;       /**< +12  Free block count. */
    uint32_t s_free_inodes_count;       /**< +16  Free inode count. */
    uint32_t s_first_data_block;        /**< +20  First data block (0 if block_size > 1024, else 1). */
    uint32_t s_log_block_size;          /**< +24  Block size = 1024 << s_log_block_size. */
    int32_t  s_log_frag_size;           /**< +28  Fragment size (signed, same formula). */
    uint32_t s_blocks_per_group;        /**< +32  Blocks per block group. */
    uint32_t s_frags_per_group;         /**< +36  Fragments per block group. */
    uint32_t s_inodes_per_group;        /**< +40  Inodes per block group. */
    uint32_t s_mtime;                   /**< +44  Last mount time (POSIX epoch). */
    uint32_t s_wtime;                   /**< +48  Last write time (POSIX epoch). */
    uint16_t s_mnt_count;               /**< +52  Mount count since last fsck. */
    int16_t  s_max_mnt_count;           /**< +54  Max mounts before forced fsck (-1 = none). */
    uint16_t s_magic;                   /**< +56  Magic number (0xEF53). */
    uint16_t s_state;                   /**< +58  Filesystem state (EXT2_FS_*). */
    uint16_t s_errors;                  /**< +60  Error handling behavior. */
    uint16_t s_minor_rev_level;         /**< +62  Minor revision level. */
    uint32_t s_lastcheck;               /**< +64  Last fsck time (POSIX epoch). */
    uint32_t s_checkinterval;           /**< +68  Interval between fscks (seconds). */
    uint32_t s_creator_os;              /**< +72  OS that created this FS. */
    uint32_t s_rev_level;               /**< +76  Revision level (0 or 1). */
    uint16_t s_def_resuid;              /**< +80  Default UID for reserved blocks. */
    uint16_t s_def_resgid;              /**< +82  Default GID for reserved blocks. */

    /* --- Extended superblock (rev >= 1) --- */
    uint32_t s_first_ino;               /**< +84  First non-reserved inode. */
    uint16_t s_inode_size;              /**< +88  Inode structure size in bytes. */
    uint16_t s_block_group_nr;          /**< +90  Block group hosting this superblock. */
    uint32_t s_feature_compat;          /**< +92  Compatible feature flags. */
    uint32_t s_feature_incompat;        /**< +96  Incompatible feature flags. */
    uint32_t s_feature_ro_compat;       /**< +100 Read-only incompatible feature flags. */
    uint8_t  s_uuid[16];               /**< +104 128-bit UUID. */
    char     s_volume_name[16];         /**< +120 Volume name (null-terminated). */
    char     s_last_mounted[64];        /**< +136 Path of last mount point. */
    uint32_t s_algo_bitmap;             /**< +200 Compression algorithm bitmap. */

    /* --- Prealloc and journal fields --- */
    uint8_t  s_prealloc_blocks;         /**< +204 Number of blocks to preallocate for files. */
    uint8_t  s_prealloc_dir_blocks;     /**< +205 Number of blocks to preallocate for dirs. */
    uint16_t s_padding1;                /**< +206 Padding to next uint32_t. */
    uint8_t  s_journal_uuid[16];        /**< +208 UUID of journal superblock. */
    uint32_t s_journal_inum;            /**< +224 Inode number of journal file. */
    uint32_t s_journal_dev;             /**< +228 Device number of journal. */
    uint32_t s_last_orphan;             /**< +232 Head of orphan inode list. */
    uint32_t s_hash_seed[4];            /**< +236 HTree hash seed. */
    uint8_t  s_def_hash_version;        /**< +252 Default hash version. */
    uint8_t  s_padding2[3];             /**< +253 Padding. */
    uint32_t s_default_mount_options;   /**< +256 Default mount options. */
    uint32_t s_first_meta_bg;           /**< +260 First meta block group. */

    /** Reserved space (zeros): offsets 264-507, 61 uint32_t entries = 244 bytes. */
    uint32_t s_reserved[61];            /**< +264 Reserved for future use. */

    /* --- Secondary magic (rev >= 1) --- */
    uint16_t s_two;                     /**< +508 Secondary magic (0xEF53). */
    uint8_t  s_minor;                   /**< +510 Minor version. */
    uint8_t  s_major;                   /**< +511 Major version. */
    uint8_t  s_reserved2[512];          /**< +512 Reserved (padding to 1024). */
} __attribute__((packed)) ext2_superblock_t;

/* =========================================================================
 * Block Group Descriptor
 *
 * One 32-byte entry per block group, stored contiguously starting at
 * block 2 (for 1K blocks). Multiple descriptors may span one block.
 * ========================================================================= */

/**
 * @brief ext2 block group descriptor — describes one block group.
 *
 * All block numbers are 32-bit per the ext2 specification.
 * Total size: 32 bytes (4+4+4+2+2+2+14 = 32).
 */
typedef struct {
    uint32_t bg_block_bitmap;       /**< +0  Block number of block bitmap. */
    uint32_t bg_inode_bitmap;       /**< +4  Block number of inode bitmap. */
    uint32_t bg_inode_table;        /**< +8  Block number of inode table. */
    uint16_t bg_free_blocks_count;  /**< +12 Free block count in this group. */
    uint16_t bg_free_inodes_count;  /**< +14 Free inode count in this group. */
    uint16_t bg_used_dirs_count;    /**< +16 Directories in this group. */
    uint8_t  bg_pad[14];           /**< +18 Reserved/padding (zeros). */
} __attribute__((packed)) ext2_group_desc_t;

/* =========================================================================
 * Inode
 *
 * 128 bytes in rev 0. Located in the inode table (bg_inode_table block).
 * Inode numbers are 1-based; inode 1 is the bad blocks inode (unused),
 * inode 2 is always the root directory.
 * ========================================================================= */

/**
 * @brief ext2 inode — on-disk representation (128 bytes).
 */
typedef struct {
    uint16_t i_mode;                /**< +0   File type (S_IF*) | permissions. */
    uint16_t i_uid;                 /**< +2   Owner UID. */
    uint32_t i_size;                /**< +4   Lower 32 bits of file size in bytes. */
    uint32_t i_atime;               /**< +8   Last access time (POSIX epoch). */
    uint32_t i_ctime;               /**< +12  Creation time. */
    uint32_t i_mtime;               /**< +16  Last modification time. */
    uint32_t i_dtime;               /**< +20  Deletion time (0 = active). */
    uint16_t i_gid;                 /**< +24  Group ID. */
    uint16_t i_links_count;         /**< +26  Hard link count. */
    uint32_t i_blocks;              /**< +28  Number of 512-byte blocks allocated. */
    uint32_t i_flags;               /**< +32  Inode flags (EXT2_IMMUTABLE_FL, etc.). */
    uint32_t i_osd1;                /**< +36  OS-dependent value 1. */
    uint32_t i_block[15];           /**< +40  Block pointers: [0..11] direct,
                                                    [12] single indirect,
                                                    [13] double indirect,
                                                    [14] triple indirect. */
    uint32_t i_generation;          /**< +100 File version (NFS). */
    uint32_t i_file_acl;            /**< +104 Extended attribute block (0 = none). */
    uint32_t i_size_high;           /**< +108 Upper 32 bits of file size. */
    uint32_t i_fsize;               /**< +112 Fragment size (unused, 0). */
    uint32_t i_osd2[3];             /**< +116 OS-dependent value 2 (12 bytes). */
} __attribute__((packed)) ext2_inode_t;

/* =========================================================================
 * Directory Entry
 *
 * Variable-length, 4-byte aligned. Stored in data blocks of a directory
 * inode. The rec_len field gives the total entry length including padding.
 * ========================================================================= */

/** Directory entry file type values (i_mode high bits). */
#define EXT2_FT_UNKNOWN     0   /**< Unknown type. */
#define EXT2_FT_REG_FILE    1   /**< Regular file. */
#define EXT2_FT_DIR         2   /**< Directory. */
#define EXT2_FT_CHRDEV      3   /**< Character device. */
#define EXT2_FT_BLKDEV      4   /**< Block device. */
#define EXT2_FT_FIFO        5   /**< FIFO. */
#define EXT2_FT_SOCK        6   /**< Socket. */
#define EXT2_FT_SYMLINK     7   /**< Symbolic link. */

/**
 * @brief ext2 directory entry — variable-length, packed.
 *
 * Total size = 8 + name_len, rounded up to next multiple of 4.
 * Minimum size = 12 (for a 1-char name, which pads to 12).
 * Inode number is 32-bit per the ext2 specification.
 */
typedef struct {
    uint32_t inode;                 /**< +0 Inode number (0 = unused entry). */
    uint16_t rec_len;               /**< +4 Total entry length in bytes (4-byte aligned). */
    uint8_t  name_len;              /**< +6 Length of name in bytes. */
    uint8_t  file_type;             /**< +7 File type (EXT2_FT_*). */
    char     name[0];               /**< +8 File name (variable, not null-terminated). */
} __attribute__((packed)) ext2_dirent_t;

/* =========================================================================
 * Block Size Helpers
 * ========================================================================= */

/**
 * @brief Compute ext2 block size from superblock field.
 * @param sb  Pointer to superblock.
 * @return Block size in bytes (1024, 2048, or 4096).
 */
#define EXT2_BLOCK_SIZE(sb)  (1024U << (sb)->s_log_block_size)

/**
 * @brief Compute inode table offset (byte) for a given inode number.
 *
 * @param sb        Superblock.
 * @param ino       1-based inode number.
 * @param igs       Number of inodes per group (from sb or group desc).
 * @return Byte offset of the inode within the inode table block(s).
 */
#define EXT2_INO_OFFSET(sb, ino, igs) \
    (((ino) - 1) % (igs) * (sb)->s_inode_size)

/**
 * @brief Compute the block group an inode belongs to.
 * @param ino   1-based inode number.
 * @param igs   Inodes per group.
 * @return 0-based block group index.
 */
#define EXT2_INO_GROUP(ino, igs)  (((ino) - 1) / (igs))

/**
 * @brief Compute how many group descriptors fit in one block.
 * @param bs  Block size in bytes.
 * @return Number of ext2_group_desc_t entries per block.
 */
#define EXT2_DESCS_PER_BLOCK(bs)  ((bs) / sizeof(ext2_group_desc_t))

/**
 * @brief Compute total block groups from geometry.
 * @param sb  Superblock.
 * @return Total number of block groups.
 */
#define EXT2_NUM_GROUPS(sb) \
    (((sb)->s_blocks_count + (sb)->s_blocks_per_group - 1) / (sb)->s_blocks_per_group)

/* =========================================================================
 * ext2 Inode Mode Bits (i_mode field)
 * ========================================================================= */

/** File type (upper 4 bits of i_mode). */
#define EXT2_S_IFIFO     0x1000   /**< FIFO. */
#define EXT2_S_IFCHR     0x2000   /**< Character device. */
#define EXT2_S_IFDIR     0x4000   /**< Directory. */
#define EXT2_S_IFBLK     0x6000   /**< Block device. */
#define EXT2_S_IFREG     0x8000   /**< Regular file. */
#define EXT2_S_IFLNK     0xA000   /**< Symbolic link. */
#define EXT2_S_IFSOCK    0xC000   /**< Socket. */

/** Permission bits (lower 9 bits of i_mode). */
#define EXT2_S_IRWXU     0x01C0   /**< User read/write/execute. */
#define EXT2_S_IRUSR     0x0100   /**< User read. */
#define EXT2_S_IWUSR     0x0080   /**< User write. */
#define EXT2_S_IXUSR     0x0040   /**< User execute. */
#define EXT2_S_IRWXG     0x0038   /**< Group read/write/execute. */
#define EXT2_S_IRWXO     0x0007   /**< Others read/write/execute. */

/** Sticky/setuid/setgid bits. */
#define EXT2_S_ISUID     0x0800   /**< Set user ID. */
#define EXT2_S_ISGID     0x0400   /**< Set group ID. */
#define EXT2_S_ISVTX     0x0200   /**< Sticky bit. */

/** File type test macros (mask i_mode with 0xF000). */
#define EXT2_S_ISDIR(m)  (((m) & 0xF000) == EXT2_S_IFDIR)
#define EXT2_S_ISREG(m)  (((m) & 0xF000) == EXT2_S_IFREG)
#define EXT2_S_ISLNK(m)  (((m) & 0xF000) == EXT2_S_IFLNK)

/* =========================================================================
 * Filesystem States
 * ========================================================================= */

#define EXT2_FS_CLEAN        0x0001   /**< Filesystem is clean. */
#define EXT2_FS_ERRORS       0x0002   /**< Filesystem has errors. */

/* =========================================================================
 * Error Handling
 * ========================================================================= */

#define EXT2_ERRORS_CONTINUE  0x0001   /**< Continue on error. */
#define EXT2_ERRORS_RO        0x0002   /**< Remount read-only on error. */
#define EXT2_ERRORS_PANIC     0x0003   /**< Kernel panic on error. */

/* =========================================================================
 * Inode Flags (i_flags field)
 * ========================================================================= */

#define EXT2_IMMUTABLE_FL     0x00000010   /**< Immutable file. */
#define EXT2_APPEND_FL        0x00000020   /**< Append-only file. */
#define EXT2_NODUMP_FL        0x00000040   /**< Do not dump (backup). */

/* =========================================================================
 * Return Codes
 * ========================================================================= */

/** ext2 operation status codes. */
typedef enum {
    EXT2_OK              =  0,   /**< Success. */
    EXT2_ERR_NOT_MOUNTED = -1,   /**< Filesystem not mounted. */
    EXT2_ERR_BAD_MAGIC   = -2,   /**< Superblock magic mismatch. */
    EXT2_ERR_BAD_SB      = -3,   /**< Superblock validation failed. */
    EXT2_ERR_IO          = -4,   /**< Underlying block device I/O error. */
    EXT2_ERR_NO_MEM      = -5,   /**< Memory allocation failed. */
    EXT2_ERR_NOT_FOUND   = -6,   /**< Path or inode not found. */
    EXT2_ERR_NO_SPACE    = -7,   /**< No free blocks or inodes. */
    EXT2_ERR_INVALID     = -8,   /**< Invalid argument. */
} ext2_status_t;

/* =========================================================================
 * Seek Modes (for lseek)
 * ========================================================================= */

#define EXT2_SEEK_SET  0   /**< Seek from beginning of file. */
#define EXT2_SEEK_CUR  1   /**< Seek from current position. */
#define EXT2_SEEK_END  2   /**< Seek from end of file. */

/* =========================================================================
 * Stat Structure (for fstat)
 * ========================================================================= */

/**
 * @brief File status information (ext2 equivalent of struct stat).
 */
typedef struct {
    uint32_t ino;       /**< Inode number. */
    uint32_t size;      /**< File size in bytes. */
    uint32_t mode;      /**< File type + permissions (EXT2_S_*). */
    uint32_t links;     /**< Hard link count. */
    uint32_t blocks;    /**< Number of 512-byte blocks allocated. */
    uint32_t uid;       /**< Owner user ID. */
    uint32_t gid;       /**< Owner group ID. */
    uint32_t atime;     /**< Last access time (POSIX epoch). */
    uint32_t mtime;     /**< Last modification time (POSIX epoch). */
    uint32_t ctime;     /**< Inode change time (POSIX epoch). */
} ext2_stat_t;

/**
 * @brief Linux-compatible directory entry for getdents.
 *
 * Packed, variable-length. d_reclen gives total entry size.
 */
typedef struct {
    uint64_t d_ino;     /**< Inode number. */
    uint64_t d_off;     /**< Offset to next dirent. */
    uint16_t d_reclen;  /**< Length of this record. */
    uint8_t  d_type;    /**< File type (EXT2_FT_*). */
    char     d_name[0]; /**< Filename (not null-terminated). */
} __attribute__((packed)) ext2_dirent64_t;

/* =========================================================================
 * In-Memory Filesystem State
 * ========================================================================= */

/** Maximum path length for ext2 operations. */
#define EXT2_MAX_PATH  256

/**
 * @brief In-memory representation of a mounted ext2 filesystem.
 *
 * Populated by ext2_mount(), freed by ext2_unmount().
 * Access via ext2_get_fs().
 */
typedef struct {
    bool mounted;                          /**< true if currently mounted. */
    uint8_t dev_id;                        /**< Block device ID. */
    ext2_superblock_t sb;                  /**< Cached superblock copy. */
    ext2_group_desc_t *gd;                 /**< Dynamically allocated group descriptors. */
    uint32_t block_size;                   /**< Computed block size (bytes). */
    uint32_t num_groups;                   /**< Total block groups. */
    uint32_t inodes_per_group;             /**< Inodes per group (from superblock). */
    uint32_t blocks_per_group;             /**< Blocks per group (from superblock). */
    uint32_t descs_per_group;              /**< Group descriptors per group. */
    uint32_t desc_blocks;                  /**< Number of blocks holding group descriptors. */
} ext2_fs_t;

/* =========================================================================
 * Public API — Filesystem Lifecycle
 * ========================================================================= */

/**
 * @brief Initialize the ext2 subsystem.
 *
 * Must be called before ext2_mount(). Stores the serial device for logging.
 * Does not allocate or read anything from disk.
 *
 * @param serial_dev  Serial device for logging. May be NULL.
 */
void ext2_init(void *serial_dev);

/**
 * @brief Mount an ext2 filesystem from a block device.
 *
 * Reads and validates the superblock, computes geometry, reads group
 * descriptors into memory. Subsequent ext2_read_block/write_block calls
 * operate on this device.
 *
 * @param dev_id  Block device ID (must be registered via block_dev_register).
 * @return EXT2_OK on success, or an error code.
 */
ext2_status_t ext2_mount(uint8_t dev_id);

/**
 * @brief Unmount the ext2 filesystem.
 *
 * Flushes all dirty bcache buffers for this device, frees the group
 * descriptor array, and clears the mounted state.
 */
void ext2_unmount(void);

/**
 * @brief Get a pointer to the mounted filesystem state.
 *
 * @return Pointer to the global ext2_fs_t, or NULL if not mounted.
 */
ext2_fs_t *ext2_get_fs(void);

/**
 * @brief Check whether an ext2 filesystem is currently mounted.
 *
 * @return true if mounted, false otherwise.
 */
bool ext2_is_mounted(void);

/* =========================================================================
 * Public API — Block I/O
 * ========================================================================= */

/**
 * @brief Read one 1024-byte ext2 block into a caller-provided buffer.
 *
 * Reads 2 consecutive 512-byte sectors via the buffer cache and assembles
 * them into the 1024-byte block. The caller must provide a buffer of at
 * least EXT2_BLOCK_SIZE(bytes).
 *
 * @param block_num   0-based block number.
 * @param buf         Destination buffer (>= block_size bytes).
 * @return EXT2_OK on success, or EXT2_ERR_IO / EXT2_ERR_NOT_MOUNTED.
 */
ext2_status_t ext2_read_block(uint32_t block_num, void *buf);

/**
 * @brief Write one 1024-byte ext2 block from a caller-provided buffer.
 *
 * Writes 2 consecutive 512-byte sectors via the buffer cache.
 * Both sectors are marked dirty in the bcache for lazy writeback.
 *
 * @param block_num   0-based block number.
 * @param buf         Source buffer (>= block_size bytes).
 * @return EXT2_OK on success, or EXT2_ERR_IO / EXT2_ERR_NOT_MOUNTED.
 */
ext2_status_t ext2_write_block(uint32_t block_num, const void *buf);

/* =========================================================================
 * Public API — Bitmap Allocator (Phase 3)
 * ========================================================================= */

/**
 * @brief Allocate a free block from the filesystem.
 *
 * Scans the block bitmap for the first free bit, marks it used, and
 * updates the group descriptor and superblock free block counts.
 *
 * @return Allocated block number (>= first_data_block), or -1 on failure.
 */
int32_t ext2_alloc_block(void);

/**
 * @brief Free a previously allocated block.
 *
 * Clears the corresponding bit in the block bitmap and updates counts.
 *
 * @param block_num  Block number to free.
 * @return EXT2_OK on success, or an error code.
 */
ext2_status_t ext2_free_block(uint32_t block_num);

/**
 * @brief Allocate a free inode from the filesystem.
 *
 * Scans the inode bitmap for the first free bit, marks it used, and
 * updates the group descriptor and superblock free inode counts.
 *
 * @return Allocated inode number (1-based), or -1 on failure.
 */
int32_t ext2_alloc_inode(void);

/**
 * @brief Free a previously allocated inode.
 *
 * Clears the corresponding bit in the inode bitmap and updates counts.
 *
 * @param ino  Inode number to free (1-based).
 * @return EXT2_OK on success, or an error code.
 */
ext2_status_t ext2_free_inode(uint32_t ino);

/* =========================================================================
 * Public API — Inode Operations (Phase 2)
 * ========================================================================= */

/**
 * @brief Read an inode from disk into a caller-provided buffer.
 *
 * Computes the inode's location from its number and the group descriptor,
 * reads the containing inode table block, and copies the inode structure.
 *
 * @param ino   1-based inode number.
 * @param out   Destination for the inode data. Must be >= sizeof(ext2_inode_t).
 * @return EXT2_OK on success, or an error code.
 */
ext2_status_t ext2_read_inode(uint32_t ino, ext2_inode_t *out);

/**
 * @brief Write an inode from a caller-provided buffer to disk.
 *
 * Computes the inode's location, reads-modify-writes the containing
 * inode table block to preserve other inodes in the same block.
 *
 * @param ino    1-based inode number.
 * @param inode  Source inode data to write.
 * @return EXT2_OK on success, or an error code.
 */
ext2_status_t ext2_write_inode(uint32_t ino, const ext2_inode_t *inode);

/**
 * @brief Map a logical block number to a physical block number within an inode.
 *
 * Handles direct pointers, single-indirect, double-indirect, and
 * triple-indirect block references. Returns 0 if the logical block
 * is not mapped (sparse hole).
 *
 * @param inode    Pointer to the inode (in-memory, not necessarily on disk).
 * @param logical  0-based logical block number within the file.
 * @return Physical block number, or 0 if unmapped.
 */
uint32_t ext2_inode_get_block(const ext2_inode_t *inode, uint32_t logical);

/**
 * @brief Allocate a new physical block and link it into an inode at a
 *        given logical block offset.
 *
 * Allocates a free block from the bitmap, links it into the inode's
 * block pointer tree (creating indirect blocks as needed), updates the
 * inode's i_blocks and i_size, and writes the inode back to disk.
 *
 * @param ino      1-based inode number (for writing back to disk).
 * @param inode    In-memory inode to modify.
 * @param logical  Logical block offset to map the new block to.
 * @return EXT2_OK on success, or an error code.
 */
ext2_status_t ext2_inode_alloc_block(uint32_t ino, ext2_inode_t *inode,
                                     uint32_t logical);

/* =========================================================================
 * Public API — File Read/Write (Phase 5)
 * ========================================================================= */

/**
 * @brief Read data from a file (by inode number) into a buffer.
 *
 * Reads up to @p count bytes starting at @p offset within the file.
 * Handles block mapping, partial blocks, and sparse files (zero-fill).
 *
 * @param ino    1-based inode number (must be a regular file).
 * @param buf    Destination buffer.
 * @param offset Byte offset within the file to start reading.
 * @param count  Maximum bytes to read.
 * @return Number of bytes actually read, or -1 on error.
 */
int64_t ext2_read_file(uint32_t ino, void *buf, uint64_t offset, uint64_t count);

/**
 * @brief Write data from a buffer into a file (by inode number).
 *
 * Writes up to @p count bytes starting at @p offset within the file.
 * Allocates new blocks as needed. Updates i_size if the write extends
 * past the current end of file.
 *
 * @param ino    1-based inode number (must be a regular file).
 * @param buf    Source buffer.
 * @param offset Byte offset within the file to start writing.
 * @param count  Number of bytes to write.
 * @return Number of bytes actually written, or -1 on error.
 */
int64_t ext2_write_file(uint32_t ino, const void *buf, uint64_t offset,
                        uint64_t count);

/* =========================================================================
 * Public API — Directory Operations (Phase 6)
 * ========================================================================= */

/**
 * @brief Look up a directory entry by name.
 *
 * @param dir_ino  Inode number of the parent directory.
 * @param name     Entry name to search for (not null-terminated on disk).
 * @return Inode number of the entry, or 0 if not found.
 */
uint32_t ext2_dir_lookup(uint32_t dir_ino, const char *name);

/**
 * @brief Add an entry to a directory.
 *
 * @param dir_ino    Inode number of the parent directory.
 * @param name       Entry name.
 * @param child_ino  Inode number of the child.
 * @param file_type  EXT2_FT_* file type.
 * @return EXT2_OK on success.
 */
ext2_status_t ext2_dir_add_entry(uint32_t dir_ino, const char *name,
                                 uint32_t child_ino, uint8_t file_type);

/**
 * @brief Remove an entry from a directory (marks inode = 0).
 *
 * @param dir_ino  Inode number of the parent directory.
 * @param name     Entry name to remove.
 * @return EXT2_OK on success, or EXT2_ERR_NOT_FOUND.
 */
ext2_status_t ext2_dir_remove_entry(uint32_t dir_ino, const char *name);

/**
 * @brief Create a new directory under a parent.
 *
 * Allocates a new inode (mode = DIR | 0755), creates "." and ".." entries,
 * and adds the entry to the parent directory. Increments parent link count.
 *
 * @param parent_ino  Inode number of the parent directory.
 * @param name        Directory name.
 * @return New directory inode number, or 0 on error.
 */
uint32_t ext2_mkdir(uint32_t parent_ino, const char *name);

/* =========================================================================
 * Public API — File Metadata, Unlink, Rmdir, Getdents, Rename (Phase 9)
 * ========================================================================= */

/**
 * @brief Get file status information by inode number.
 *
 * @param ino  1-based inode number.
 * @param st   Output stat structure.
 * @return EXT2_OK on success.
 */
ext2_status_t ext2_stat(uint32_t ino, ext2_stat_t *st);

/**
 * @brief Remove a file from its parent directory.
 *
 * Decrements the inode's link count. If links reach 0, frees the inode
 * and all its data blocks.
 *
 * @param parent_ino  Parent directory inode number.
 * @param name        Filename to remove.
 * @return EXT2_OK on success.
 */
ext2_status_t ext2_unlink(uint32_t parent_ino, const char *name);

/**
 * @brief Remove an empty directory.
 *
 * Verifies the directory contains only "." and "..", then removes it
 * from the parent and frees its inode/blocks.
 *
 * @param parent_ino  Parent directory inode number.
 * @param name        Directory name to remove.
 * @return EXT2_OK on success, or EXT2_ERR_INVALID if not empty.
 */
ext2_status_t ext2_rmdir(uint32_t parent_ino, const char *name);

/**
 * @brief Read directory entries into a buffer (getdents interface).
 *
 * @param dir_ino  Directory inode number.
 * @param cookie   Byte offset within directory (in/out). Start at 0.
 * @param buf      Output buffer for ext2_dirent64_t entries.
 * @param count    Size of output buffer.
 * @return Number of bytes written to buf, or 0 if no more entries.
 */
int32_t ext2_getdents(uint32_t dir_ino, uint64_t *cookie,
                      void *buf, uint32_t count);

/**
 * @brief Rename/move a file between directories (or within same dir).
 *
 * @param old_dir   Source parent inode.
 * @param old_name  Source filename.
 * @param new_dir   Destination parent inode.
 * @param new_name  Destination filename.
 * @return EXT2_OK on success.
 */
ext2_status_t ext2_rename(uint32_t old_dir, const char *old_name,
                          uint32_t new_dir, const char *new_name);

/**
 * @brief Truncate a file to a specified size.
 *
 * If shrinking, frees blocks past the new end and zeros the tail.
 * If extending, zero-fills the new region.
 *
 * @param ino       Inode number.
 * @param new_size  New file size in bytes.
 * @return EXT2_OK on success.
 */
ext2_status_t ext2_truncate(uint32_t ino, uint64_t new_size);

/**
 * @brief Free all data blocks of an inode (direct + indirect).
 *
 * Does NOT modify the inode's i_size or i_blocks — caller must do that.
 *
 * @param ino    Inode number (for writing indirect block frees).
 * @param inode  In-memory inode with block pointers.
 * @return EXT2_OK on success.
 */
ext2_status_t ext2_free_all_blocks(uint32_t ino, ext2_inode_t *inode);

/* =========================================================================
 * Public API — Path Resolution (Phase 7)
 * ========================================================================= */

/**
 * @brief Resolve an absolute path to an inode number.
 *
 * @param path  Absolute path starting with '/' (or "/" for root).
 * @return Inode number, or 0 if not found.
 */
uint32_t ext2_resolve_path(const char *path);

/**
 * @brief Split a path into parent directory and final component.
 *
 * Examples:
 *   "/foo/bar"    → parent="/foo",  name="bar"
 *   "/foo"        → parent="/",     name="foo"
 *   "/"           → parent="/",     name=""
 *   "file.txt"    → parent="/",     name="file.txt"
 *
 * @param path    Path to split.
 * @param parent  Output buffer for parent path (>= EXT2_MAX_PATH bytes).
 * @param name    Output buffer for final component (>= EXT2_MAX_PATH bytes).
 */
void ext2_split_path(const char *path, char *parent, char *name);

/* =========================================================================
 * Public API — File Descriptor Integration (Phase 8)
 * ========================================================================= */

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0100

/**
 * @brief Per-fd state for ext2 file operations.
 */
typedef struct {
    uint32_t ino;       /**< Inode number. */
    uint64_t offset;    /**< Current read/write position. */
    uint32_t flags;     /**< O_RDONLY / O_WRONLY / O_RDWR. */
} ext2_file_t;

/**
 * @brief Global file operations vtable for ext2 files.
 * Used by syscall.c to wire fd_table entries.
 */
extern file_ops_t ext2_file_ops;

/**
 * @brief Global VFS operations vtable for ext2.
 * Used by the VFS mount layer to dispatch path-based operations.
 */
extern vfs_fs_ops_t ext2_vfs_ops;

/* =========================================================================
 * Public API — Mount Root Directory Initialization
 * ========================================================================= */

/**
 * @brief Initialize inode 2 (root directory) with "." and ".." entries.
 *
 * Creates a root directory containing the two mandatory entries:
 *   "."  → inode 2 (self)
 *   ".." → inode 2 (parent is self for root)
 *
 * Only call this on a freshly formatted filesystem (no existing root dir).
 * If the root dir already has content, this will corrupt it.
 *
 * @return EXT2_OK on success.
 */
ext2_status_t ext2_init_root_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_EXT2_H */
