/**
 * @file ext2.c
 * @brief ext2 filesystem — implementation.
 *
 * Provides the core ext2 operations built on top of the block device
 * layer and buffer cache:
 *   - Mount/unmount (superblock + group descriptor management)
 *   - Block I/O (1024-byte ext2 blocks via 512-byte bcache sectors)
 *   - Root directory initialization
 *
 * All block numbers in this file are ext2 logical block numbers (0-based).
 * To convert to sector numbers for bcache, multiply by 2 (for 1K blocks).
 *
 * Memory allocation:
 *   - Group descriptors: allocated via kmalloc (freed via kfree on unmount)
 *   - Temporary block buffers: stack-allocated (2048 bytes for 1K blocks)
 */

#include "ext2.h"
#include "bcache.h"
#include "vfs.h"
#include "block_dev.h"
#include "../drivers/serial.h"
#include "../lib/print.h"
#include "../kernel/kheap.h"
#include "../kernel/task.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Global State
 * ========================================================================= */

/** Singleton filesystem state. Only one mount at a time. */
static ext2_fs_t g_fs;

/** Serial device for logging. */
static serial_dev_t *g_ext2_serial = NULL;

/* =========================================================================
 * Logging
 * ========================================================================= */

static void ext2_log(const char *msg) {
    if (g_ext2_serial) serial_write_string(g_ext2_serial, msg);
}

static void ext2_log_hex32(uint32_t v) {
    if (g_ext2_serial) print_hex32(g_ext2_serial, v);
}

/* =========================================================================
 * Initialization
 * ========================================================================= */

void ext2_init(void *serial_dev) {
    g_ext2_serial = (serial_dev_t *)serial_dev;
    memset(&g_fs, 0, sizeof(g_fs));
    ext2_log("[EXT2] Init complete.\r\n");
}

/* =========================================================================
 * Block I/O — Read/Write 1024-byte ext2 blocks
 *
 * ext2 blocks are 1024 bytes; bcache sectors are 512 bytes.
 * One ext2 block = 2 consecutive bcache sectors.
 * ========================================================================= */

/** Raw block read — does not check mounted flag. Used during mount. */
static ext2_status_t ext2_read_block_raw(uint8_t dev_id, uint32_t block_num, void *buf) {
    if (!buf) return EXT2_ERR_INVALID;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t sector_base = block_num * 2;

    buf_t *b_lo = bcache_get(dev_id, sector_base);
    if (!b_lo) return EXT2_ERR_IO;
    memcpy(dst, b_lo->data, 512);
    bcache_put(b_lo);

    buf_t *b_hi = bcache_get(dev_id, sector_base + 1);
    if (!b_hi) return EXT2_ERR_IO;
    memcpy(dst + 512, b_hi->data, 512);
    bcache_put(b_hi);

    return EXT2_OK;
}

ext2_status_t ext2_read_block(uint32_t block_num, void *buf) {
    if (!g_fs.mounted) return EXT2_ERR_NOT_MOUNTED;
    if (!buf) return EXT2_ERR_INVALID;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t sector_base = block_num * 2;

    /* Read low sector (first 512 bytes). */
    buf_t *b_lo = bcache_get(g_fs.dev_id, sector_base);
    if (!b_lo) return EXT2_ERR_IO;
    memcpy(dst, b_lo->data, 512);
    bcache_put(b_lo);

    /* Read high sector (second 512 bytes). */
    buf_t *b_hi = bcache_get(g_fs.dev_id, sector_base + 1);
    if (!b_hi) return EXT2_ERR_IO;
    memcpy(dst + 512, b_hi->data, 512);
    bcache_put(b_hi);

    return EXT2_OK;
}

ext2_status_t ext2_write_block(uint32_t block_num, const void *buf) {
    if (!g_fs.mounted) return EXT2_ERR_NOT_MOUNTED;
    if (!buf) return EXT2_ERR_INVALID;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t sector_base = block_num * 2;

    /* Write low sector. */
    buf_t *b_lo = bcache_get(g_fs.dev_id, sector_base);
    if (!b_lo) return EXT2_ERR_IO;
    memcpy(b_lo->data, src, 512);
    bcache_dirty(b_lo);
    bcache_put(b_lo);

    /* Write high sector. */
    buf_t *b_hi = bcache_get(g_fs.dev_id, sector_base + 1);
    if (!b_hi) return EXT2_ERR_IO;
    memcpy(b_hi->data, src + 512, 512);
    bcache_dirty(b_hi);
    bcache_put(b_hi);

    return EXT2_OK;
}

/* =========================================================================
 * Mount / Unmount
 * ========================================================================= */

ext2_status_t ext2_mount(uint8_t dev_id) {
    if (g_fs.mounted) {
        ext2_log("[EXT2] ERR: already mounted\r\n");
        return EXT2_ERR_INVALID;
    }

    g_fs.dev_id = dev_id;

    /* --- Read superblock (block 1, sectors 2-3) --- */
    ext2_superblock_t tmp_sb;
    ext2_status_t st = ext2_read_block_raw(dev_id, 1, &tmp_sb);
    if (st != EXT2_OK) return st;

    /* Validate magic. */
    if (tmp_sb.s_magic != EXT2_SUPER_MAGIC) {
        ext2_log("[EXT2] ERR: bad magic\r\n");
        return EXT2_ERR_BAD_MAGIC;
    }

    /* Validate block size (must be 1024, 2048, or 4096). */
    if (tmp_sb.s_log_block_size > 2) {
        ext2_log("[EXT2] ERR: unsupported block size\r\n");
        return EXT2_ERR_BAD_SB;
    }

    /* Validate inode size (128 for rev 0, must be power of 2). */
    if (tmp_sb.s_inode_size == 0) tmp_sb.s_inode_size = EXT2_INODE_SIZE;
    if (tmp_sb.s_inode_size < 128 || tmp_sb.s_inode_size > 4096) {
        ext2_log("[EXT2] ERR: bad inode size\r\n");
        return EXT2_ERR_BAD_SB;
    }

    /* Store superblock. */
    memcpy(&g_fs.sb, &tmp_sb, sizeof(ext2_superblock_t));

    /* Compute geometry. */
    g_fs.block_size       = EXT2_BLOCK_SIZE(&g_fs.sb);
    g_fs.blocks_per_group = g_fs.sb.s_blocks_per_group;
    g_fs.inodes_per_group = g_fs.sb.s_inodes_per_group;
    g_fs.num_groups       = EXT2_NUM_GROUPS(&g_fs.sb);
    g_fs.descs_per_group  = EXT2_DESCS_PER_BLOCK(g_fs.block_size);

    /* Compute how many blocks the group descriptor table occupies. */
    uint32_t total_descs = g_fs.num_groups;
    g_fs.desc_blocks = (total_descs + g_fs.descs_per_group - 1)
                     / g_fs.descs_per_group;

    /* --- Read group descriptors (starts at block 2 for 1K blocks) --- */
    uint32_t gd_bytes = g_fs.desc_blocks * g_fs.block_size;
    g_fs.gd = (ext2_group_desc_t *)kmalloc(gd_bytes);
    if (!g_fs.gd) {
        ext2_log("[EXT2] ERR: no memory for group descriptors\r\n");
        return EXT2_ERR_NO_MEM;
    }

    /* Read descriptor blocks. */
    for (uint32_t b = 0; b < g_fs.desc_blocks; b++) {
        uint32_t offset = b * g_fs.block_size;
        uint32_t remaining = gd_bytes - offset;
        uint32_t chunk = (remaining < g_fs.block_size) ? remaining : g_fs.block_size;

        uint8_t tmp_block[1024];
        st = ext2_read_block_raw(dev_id, 2 + b, tmp_block);
        if (st != EXT2_OK) {
            kfree(g_fs.gd);
            g_fs.gd = NULL;
            return st;
        }
        memcpy(((uint8_t *)g_fs.gd) + offset, tmp_block, chunk);
    }

    g_fs.mounted = true;

    ext2_log("[EXT2] Mounted: groups=");
    /* Print number of groups (simple decimal). */
    char numbuf[8];
    uint32_t n = g_fs.num_groups;
    int len = 0;
    if (n == 0) { numbuf[len++] = '0'; }
    else {
        char tmp[8];
        int tlen = 0;
        while (n > 0) { tmp[tlen++] = '0' + (n % 10); n /= 10; }
        for (int j = tlen - 1; j >= 0; j--) numbuf[len++] = tmp[j];
    }
    numbuf[len] = '\0';
    ext2_log(numbuf);
    ext2_log(" bs=");
    n = g_fs.block_size;
    len = 0;
    if (n == 0) { numbuf[len++] = '0'; }
    else {
        char tmp[8];
        int tlen = 0;
        while (n > 0) { tmp[tlen++] = '0' + (n % 10); n /= 10; }
        for (int j = tlen - 1; j >= 0; j--) numbuf[len++] = tmp[j];
    }
    numbuf[len] = '\0';
    ext2_log(numbuf);
    ext2_log("\r\n");

    return EXT2_OK;
}

void ext2_unmount(void) {
    if (!g_fs.mounted) return;

    /* Flush all dirty buffers for this device. */
    bcache_flush_dev(g_fs.dev_id);

    /* Free group descriptor array. */
    if (g_fs.gd) {
        kfree(g_fs.gd);
        g_fs.gd = NULL;
    }

    g_fs.mounted = false;
    ext2_log("[EXT2] Unmounted.\r\n");
}

ext2_fs_t *ext2_get_fs(void) {
    return g_fs.mounted ? &g_fs : NULL;
}

bool ext2_is_mounted(void) {
    return g_fs.mounted;
}

/* =========================================================================
 * Inode Operations
 *
 * Inodes are 1-based. Inode 1 is the bad blocks inode (unused).
 * Inode 2 is the root directory.
 *
 * Location on disk:
 *   group = (ino - 1) / inodes_per_group
 *   index = (ino - 1) % inodes_per_group
 *   byte_offset = index * inode_size  (within the inode table)
 *   table_block = gd[group].bg_inode_table
 *   block_within_table = byte_offset / block_size
 *   byte_within_block  = byte_offset % block_size
 * ========================================================================= */

/**
 * Compute disk location of an inode.
 * Returns EXT2_OK on success, or EXT2_ERR_INVALID if the inode is out of range.
 */
static ext2_status_t ext2_inode_location(uint32_t ino, uint32_t *table_block,
                                         uint32_t *block_off, uint32_t *byte_off) {
    if (ino == 0) return EXT2_ERR_INVALID;

    uint32_t group = EXT2_INO_GROUP(ino, g_fs.inodes_per_group);
    if (group >= g_fs.num_groups) return EXT2_ERR_INVALID;

    uint32_t index = (ino - 1) % g_fs.inodes_per_group;
    uint32_t byte_offset = index * g_fs.sb.s_inode_size;

    *table_block = g_fs.gd[group].bg_inode_table;
    *block_off   = byte_offset / g_fs.block_size;
    *byte_off    = byte_offset % g_fs.block_size;
    return EXT2_OK;
}

ext2_status_t ext2_read_inode(uint32_t ino, ext2_inode_t *out) {
    if (!g_fs.mounted) return EXT2_ERR_NOT_MOUNTED;
    if (!out) return EXT2_ERR_INVALID;

    uint32_t table_block, block_off, byte_off;
    ext2_status_t st = ext2_inode_location(ino, &table_block, &block_off, &byte_off);
    if (st != EXT2_OK) return st;

    uint8_t buf[1024];
    st = ext2_read_block(table_block + block_off, buf);
    if (st != EXT2_OK) return st;

    memcpy(out, buf + byte_off, sizeof(ext2_inode_t));
    return EXT2_OK;
}

ext2_status_t ext2_write_inode(uint32_t ino, const ext2_inode_t *inode) {
    if (!g_fs.mounted) return EXT2_ERR_NOT_MOUNTED;
    if (!inode) return EXT2_ERR_INVALID;

    uint32_t table_block, block_off, byte_off;
    ext2_status_t st = ext2_inode_location(ino, &table_block, &block_off, &byte_off);
    if (st != EXT2_OK) return st;

    uint8_t buf[1024];
    st = ext2_read_block(table_block + block_off, buf);
    if (st != EXT2_OK) return st;

    memcpy(buf + byte_off, inode, sizeof(ext2_inode_t));

    st = ext2_write_block(table_block + block_off, buf);
    return st;
}

/* =========================================================================
 * Inode Block Mapping
 *
 * ext2 inodes have 15 block pointers: [0..11] direct, [12] single
 * indirect, [13] double indirect, [14] triple indirect.
 *
 * For 1K blocks, each indirect block holds 256 uint32_t pointers.
 *   Single indirect:  256 blocks
 *   Double indirect:  256 * 256 = 65536 blocks
 *   Triple indirect:  256 * 256 * 256 = 16,777,216 blocks
 *
 * The pointers in indirect blocks are stored in 1K ext2 blocks (same
 * as all other blocks). We read them via ext2_read_block.
 * ========================================================================= */

/** Pointers per indirect block for the current block size. */
#define EXT2_PTRS_PER_BLOCK  (g_fs.block_size / sizeof(uint32_t))

uint32_t ext2_inode_get_block(const ext2_inode_t *inode, uint32_t logical) {
    if (!inode) return 0;

    uint32_t ptrs = EXT2_PTRS_PER_BLOCK;

    /* Direct pointers: [0..11] */
    if (logical < EXT2_N_DIRECT) {
        return inode->i_block[logical];
    }
    logical -= EXT2_N_DIRECT;

    /* Single indirect: [12] */
    if (logical < ptrs) {
        if (inode->i_block[12] == 0) return 0;
        uint32_t buf[256]; /* 1K / 4 = 256 ptrs */
        if (ext2_read_block(inode->i_block[12], buf) != EXT2_OK) return 0;
        return buf[logical];
    }
    logical -= ptrs;

    /* Double indirect: [13] */
    if (logical < ptrs * ptrs) {
        if (inode->i_block[13] == 0) return 0;
        uint32_t L1[256];
        if (ext2_read_block(inode->i_block[13], L1) != EXT2_OK) return 0;
        uint32_t idx1 = logical / ptrs;
        uint32_t idx0 = logical % ptrs;
        if (L1[idx1] == 0) return 0;
        uint32_t L0[256];
        if (ext2_read_block(L1[idx1], L0) != EXT2_OK) return 0;
        return L0[idx0];
    }
    logical -= ptrs * ptrs;

    /* Triple indirect: [14] */
    if (inode->i_block[14] == 0) return 0;
    uint32_t L2[256];
    if (ext2_read_block(inode->i_block[14], L2) != EXT2_OK) return 0;
    uint32_t idx2 = logical / (ptrs * ptrs);
    uint32_t rem  = logical % (ptrs * ptrs);
    if (L2[idx2] == 0) return 0;
    uint32_t L1[256];
    if (ext2_read_block(L2[idx2], L1) != EXT2_OK) return 0;
    uint32_t idx1 = rem / ptrs;
    uint32_t idx0 = rem % ptrs;
    if (L1[idx1] == 0) return 0;
    uint32_t L0[256];
    if (ext2_read_block(L1[idx1], L0) != EXT2_OK) return 0;
    return L0[idx0];
}

/**
 * Read an indirect block into buf. Returns EXT2_OK or error.
 */
static ext2_status_t ext2_read_indirect(uint32_t blk, uint32_t *buf) {
    if (blk == 0) return EXT2_ERR_IO;
    return ext2_read_block(blk, buf);
}

/**
 * Write an indirect block from buf. Returns EXT2_OK or error.
 */
static ext2_status_t ext2_write_indirect(uint32_t blk, const uint32_t *buf) {
    if (blk == 0) return EXT2_ERR_IO;
    return ext2_write_block(blk, buf);
}

ext2_status_t ext2_inode_alloc_block(uint32_t ino, ext2_inode_t *inode,
                                     uint32_t logical) {
    if (!inode) return EXT2_ERR_INVALID;

    int32_t new_block = ext2_alloc_block();
    if (new_block < 0) return EXT2_ERR_NO_SPACE;

    uint32_t phys = (uint32_t)new_block;
    uint32_t ptrs = EXT2_PTRS_PER_BLOCK;

    if (logical < EXT2_N_DIRECT) {
        inode->i_block[logical] = phys;
    } else if (logical < EXT2_N_DIRECT + ptrs) {
        /* Single indirect. */
        uint32_t idx = logical - EXT2_N_DIRECT;
        if (inode->i_block[12] == 0) {
            /* Allocate the indirect block itself. */
            int32_t iblk = ext2_alloc_block();
            if (iblk < 0) { ext2_free_block(phys); return EXT2_ERR_NO_SPACE; }
            inode->i_block[12] = (uint32_t)iblk;
            /* Zero the indirect block. */
            uint8_t zero_buf[1024];
            memset(zero_buf, 0, g_fs.block_size);
            ext2_write_block((uint32_t)iblk, zero_buf);
            inode->i_blocks += g_fs.block_size / 512;
        }
        uint32_t ind[256];
        ext2_read_block(inode->i_block[12], ind);
        ind[idx] = phys;
        ext2_write_block(inode->i_block[12], ind);
    } else if (logical < EXT2_N_DIRECT + ptrs + ptrs * ptrs) {
        /* Double indirect. */
        uint32_t rem = logical - EXT2_N_DIRECT - ptrs;
        uint32_t idx1 = rem / ptrs;
        uint32_t idx0 = rem % ptrs;

        if (inode->i_block[13] == 0) {
            int32_t iblk = ext2_alloc_block();
            if (iblk < 0) { ext2_free_block(phys); return EXT2_ERR_NO_SPACE; }
            inode->i_block[13] = (uint32_t)iblk;
            uint8_t zero_buf[1024];
            memset(zero_buf, 0, g_fs.block_size);
            ext2_write_block((uint32_t)iblk, zero_buf);
            inode->i_blocks += g_fs.block_size / 512;
        }
        uint32_t L1[256];
        ext2_read_block(inode->i_block[13], L1);

        if (L1[idx1] == 0) {
            int32_t iblk = ext2_alloc_block();
            if (iblk < 0) { ext2_free_block(phys); return EXT2_ERR_NO_SPACE; }
            L1[idx1] = (uint32_t)iblk;
            uint8_t zero_buf[1024];
            memset(zero_buf, 0, g_fs.block_size);
            ext2_write_block((uint32_t)iblk, zero_buf);
            ext2_write_block(inode->i_block[13], L1);
            inode->i_blocks += g_fs.block_size / 512;
        }

        uint32_t L0[256];
        ext2_read_block(L1[idx1], L0);
        L0[idx0] = phys;
        ext2_write_block(L1[idx1], L0);
    } else {
        /* Triple indirect. */
        uint32_t rem = logical - EXT2_N_DIRECT - ptrs - ptrs * ptrs;
        uint32_t idx2 = rem / (ptrs * ptrs);
        uint32_t r1   = rem % (ptrs * ptrs);
        uint32_t idx1 = r1 / ptrs;
        uint32_t idx0 = r1 % ptrs;

        if (inode->i_block[14] == 0) {
            int32_t iblk = ext2_alloc_block();
            if (iblk < 0) { ext2_free_block(phys); return EXT2_ERR_NO_SPACE; }
            inode->i_block[14] = (uint32_t)iblk;
            uint8_t zero_buf[1024];
            memset(zero_buf, 0, g_fs.block_size);
            ext2_write_block((uint32_t)iblk, zero_buf);
            inode->i_blocks += g_fs.block_size / 512;
        }
        uint32_t L2[256];
        ext2_read_block(inode->i_block[14], L2);

        if (L2[idx2] == 0) {
            int32_t iblk = ext2_alloc_block();
            if (iblk < 0) { ext2_free_block(phys); return EXT2_ERR_NO_SPACE; }
            L2[idx2] = (uint32_t)iblk;
            uint8_t zero_buf[1024];
            memset(zero_buf, 0, g_fs.block_size);
            ext2_write_block((uint32_t)iblk, zero_buf);
            ext2_write_block(inode->i_block[14], L2);
            inode->i_blocks += g_fs.block_size / 512;
        }

        uint32_t L1[256];
        ext2_read_block(L2[idx2], L1);

        if (L1[idx1] == 0) {
            int32_t iblk = ext2_alloc_block();
            if (iblk < 0) { ext2_free_block(phys); return EXT2_ERR_NO_SPACE; }
            L1[idx1] = (uint32_t)iblk;
            uint8_t zero_buf[1024];
            memset(zero_buf, 0, g_fs.block_size);
            ext2_write_block((uint32_t)iblk, zero_buf);
            ext2_write_block(L2[idx2], L1);
            inode->i_blocks += g_fs.block_size / 512;
        }

        uint32_t L0[256];
        ext2_read_block(L1[idx1], L0);
        L0[idx0] = phys;
        ext2_write_block(L1[idx1], L0);
    }

    /* Update i_blocks (in 512-byte units) and persist. */
    inode->i_blocks += g_fs.block_size / 512;
    return ext2_write_inode(ino, inode);
}

/* =========================================================================
 * File Read/Write
 *
 * Reads and writes file data by inode number. Handles block mapping,
 * partial blocks, sparse files, and offset/size management.
 * ========================================================================= */

/** Return the smaller of two uint64 values. */
static uint64_t ext2_min64(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

int64_t ext2_read_file(uint32_t ino, void *buf, uint64_t offset, uint64_t count) {
    if (!g_fs.mounted) return -1;
    if (!buf && count > 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != EXT2_OK) return -1;

    uint64_t file_size = (uint64_t)inode.i_size;
    ext2_log("[EXT2] read_file: ino="); ext2_log_hex32(ino);
    ext2_log(" offset="); print_hex64(g_ext2_serial, offset);
    ext2_log(" count="); print_hex64(g_ext2_serial, count);
    ext2_log(" i_size="); print_hex64(g_ext2_serial, file_size);
    ext2_log(" i_blocks="); ext2_log_hex32(inode.i_blocks);
    ext2_log(" i_block[0]="); ext2_log_hex32(inode.i_block[0]);
    ext2_log("\r\n");

    if (offset >= file_size) return 0;

    /* Clamp count to end of file. */
    count = ext2_min64(count, file_size - offset);
    if (count == 0) return 0;

    uint64_t total_read = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (count > 0) {
        uint32_t logical = (uint32_t)(offset / g_fs.block_size);
        uint32_t block_off = (uint32_t)(offset % g_fs.block_size);
        uint32_t to_read = (uint32_t)ext2_min64(count,
                                                  g_fs.block_size - block_off);

        uint32_t phys = ext2_inode_get_block(&inode, logical);
        ext2_log("[EXT2] read_file: logical="); ext2_log_hex32(logical);
        ext2_log(" phys="); ext2_log_hex32(phys);
        ext2_log(" to_read="); ext2_log_hex32(to_read);
        ext2_log("\r\n");

        if (phys == 0) {
            ext2_log("[EXT2] read_file: sparse hole\r\n");
            memset(dst, 0, to_read);
        } else {
            uint8_t blk_buf[1024];
            if (ext2_read_block(phys, blk_buf) != EXT2_OK) {
                ext2_log("[EXT2] read_file: read_block FAILED phys="); ext2_log_hex32(phys); ext2_log("\r\n");
                return (int64_t)total_read;
            }
            memcpy(dst, blk_buf + block_off, to_read);
        }

        dst        += to_read;
        offset     += to_read;
        count      -= to_read;
        total_read += to_read;
    }

    ext2_log("[EXT2] read_file: returning "); print_hex64(g_ext2_serial, (uint64_t)total_read); ext2_log("\r\n");
    return (int64_t)total_read;
}

int64_t ext2_write_file(uint32_t ino, const void *buf, uint64_t offset,
                        uint64_t count) {
    if (!g_fs.mounted) return -1;
    if (!buf && count > 0) return -1;

    ext2_log("[EXT2] write_file: ino="); ext2_log_hex32(ino);
    ext2_log(" offset="); print_hex64(g_ext2_serial, offset);
    ext2_log(" count="); print_hex64(g_ext2_serial, count);
    ext2_log("\r\n");

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != EXT2_OK) {
        ext2_log("[EXT2] write_file: read_inode FAILED\r\n");
        return -1;
    }

    ext2_log("[EXT2] write_file: i_size="); ext2_log_hex32(inode.i_size);
    ext2_log(" i_blocks="); ext2_log_hex32(inode.i_blocks);
    ext2_log("\r\n");

    uint64_t total_written = 0;
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t pos = offset;

    while (count > 0) {
        uint32_t logical = (uint32_t)(pos / g_fs.block_size);
        uint32_t block_off = (uint32_t)(pos % g_fs.block_size);
        uint32_t to_write = (uint32_t)ext2_min64(count,
                                                   g_fs.block_size - block_off);

        uint32_t phys = ext2_inode_get_block(&inode, logical);
        ext2_log("[EXT2] write_file: logical="); ext2_log_hex32(logical);
        ext2_log(" phys="); ext2_log_hex32(phys);
        ext2_log(" to_write="); ext2_log_hex32(to_write);
        ext2_log("\r\n");

        /* Allocate a block if this logical position is unmapped. */
        if (phys == 0) {
            ext2_log("[EXT2] write_file: allocating block for logical="); ext2_log_hex32(logical); ext2_log("\r\n");
            ext2_status_t st = ext2_inode_alloc_block(ino, &inode, logical);
            if (st != EXT2_OK) {
                ext2_log("[EXT2] write_file: alloc_block FAILED\r\n");
                return (int64_t)total_written;
            }
            if (ext2_read_inode(ino, &inode) != EXT2_OK) {
                ext2_log("[EXT2] write_file: re-read inode FAILED\r\n");
                return (int64_t)total_written;
            }
            phys = ext2_inode_get_block(&inode, logical);
            ext2_log("[EXT2] write_file: allocated phys="); ext2_log_hex32(phys); ext2_log("\r\n");
            if (phys == 0) {
                ext2_log("[EXT2] write_file: phys still zero after alloc!\r\n");
                return (int64_t)total_written;
            }
        }

        uint8_t blk_buf[1024];

        /* Read-modify-write for partial block operations. */
        if (block_off > 0 || to_write < g_fs.block_size) {
            if (ext2_read_block(phys, blk_buf) != EXT2_OK)
                return (int64_t)total_written;
        }

        memcpy(blk_buf + block_off, src, to_write);

        if (ext2_write_block(phys, blk_buf) != EXT2_OK)
            return (int64_t)total_written;

        src           += to_write;
        pos           += to_write;
        count         -= to_write;
        total_written += to_write;
    }

    /* Update file size if the write extended past the current end. */
    if (pos > (uint64_t)inode.i_size) {
        inode.i_size = (uint32_t)pos;
        ext2_log("[EXT2] write_file: updating i_size to "); ext2_log_hex32(inode.i_size); ext2_log("\r\n");
        ext2_write_inode(ino, &inode);
    } else {
        ext2_log("[EXT2] write_file: i_size unchanged (pos="); print_hex64(g_ext2_serial, pos); ext2_log(" <= i_size="); ext2_log_hex32(inode.i_size); ext2_log(")\r\n");
    }

    ext2_log("[EXT2] write_file: returning "); print_hex64(g_ext2_serial, (uint64_t)total_written); ext2_log("\r\n");
    return (int64_t)total_written;
}

/* =========================================================================
 * Bitmap Allocator
 *
 * Block bitmap: one bit per block in the group. Bit 0 = first block
 * in the group. For 1K blocks with 256 blocks/group, the bitmap fits
 * in one block (256 bits = 32 bytes).
 *
 * Inode bitmap: one bit per inode in the group. Bit 0 = inode 1
 * (first inode in the group). For 4096 inodes/group, the bitmap
 * fits in one block (4096 bits = 512 bytes).
 * ========================================================================= */

/** Set a bit in a bitmap byte array. */
static void bitmap_set(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (1U << (bit % 8));
}

/** Clear a bit in a bitmap byte array. */
static void bitmap_clear(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] &= ~(1U << (bit % 8));
}

/** Test a bit in a bitmap byte array. Returns 1 if set, 0 if clear. */
static int bitmap_test(const uint8_t *bitmap, uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

/**
 * Find the first zero bit (free resource) in a bitmap.
 * @param bitmap       Pointer to the bitmap data (in a block buffer).
 * @param total_bits   Total number of bits to scan.
 * @return Bit index of the first free resource, or -1 if none found.
 */
static int32_t bitmap_find_free(const uint8_t *bitmap, uint32_t total_bits) {
    uint32_t full_bytes = total_bits / 8;
    uint32_t remaining  = total_bits % 8;

    /* Scan full bytes for a non-0xFF byte. */
    for (uint32_t i = 0; i < full_bytes; i++) {
        if (bitmap[i] != 0xFF) {
            for (int b = 0; b < 8; b++) {
                if (!(bitmap[i] & (1U << b)))
                    return (int32_t)(i * 8 + b);
            }
        }
    }
    /* Scan remaining bits. */
    for (uint32_t b = 0; b < remaining; b++) {
        if (!(bitmap[full_bytes] & (1U << b)))
            return (int32_t)(full_bytes * 8 + b);
    }
    return -1;
}

/**
 * Update free counts in the group descriptor and superblock,
 * then write both back to disk.
 *
 * @param group         Block group index.
 * @param delta_blocks  Change in free block count (positive = free, negative = alloc).
 * @param delta_inodes  Change in free inode count (positive = free, negative = alloc).
 */
static ext2_status_t ext2_update_counts(uint32_t group, int32_t delta_blocks,
                                        int32_t delta_inodes) {
    ext2_status_t st;

    /* Update in-memory group descriptor. */
    g_fs.gd[group].bg_free_blocks_count = (uint16_t)(
        g_fs.gd[group].bg_free_blocks_count + delta_blocks);
    g_fs.gd[group].bg_free_inodes_count = (uint16_t)(
        g_fs.gd[group].bg_free_inodes_count + delta_inodes);

    /* Write group descriptor back to disk (block 2). */
    st = ext2_write_block(2, g_fs.gd);
    if (st != EXT2_OK) return st;

    /* Update in-memory superblock. */
    g_fs.sb.s_free_blocks_count = (uint32_t)(
        (int32_t)g_fs.sb.s_free_blocks_count + delta_blocks);
    g_fs.sb.s_free_inodes_count = (uint32_t)(
        (int32_t)g_fs.sb.s_free_inodes_count + delta_inodes);

    /* Write superblock back to disk (block 1). */
    st = ext2_write_block(1, &g_fs.sb);
    return st;
}

int32_t ext2_alloc_block(void) {
    if (!g_fs.mounted) return -1;

    /* Scan all groups for a free block. We only have 1 group. */
    for (uint32_t g = 0; g < g_fs.num_groups; g++) {
        uint32_t bitmap_block = g_fs.gd[g].bg_block_bitmap;
        uint32_t blocks_in_group = g_fs.blocks_per_group;
        /* In group 0, first usable bit is s_first_data_block + 1.
         * For 1K blocks, s_first_data_block = 1, so first usable = bit 2.
         * But for simplicity, scan from bit 0; already-used bits are set. */
        if (g == 0 && blocks_in_group > g_fs.sb.s_first_data_block) {
            blocks_in_group = g_fs.sb.s_first_data_block;
            /* Actually scan the full group. The bitmap already has
             * the right bits set for blocks 0-12. */
            blocks_in_group = g_fs.blocks_per_group;
        }

        uint8_t buf[1024];
        ext2_status_t st = ext2_read_block(bitmap_block, buf);
        if (st != EXT2_OK) return -1;

        int32_t bit = bitmap_find_free(buf, g_fs.blocks_per_group);
        if (bit < 0) continue;  /* This group is full. */

        /* Mark the block as used. */
        bitmap_set(buf, (uint32_t)bit);
        st = ext2_write_block(bitmap_block, buf);
        if (st != EXT2_OK) return -1;

        /* Update counts. */
        st = ext2_update_counts(g, -1, 0);
        if (st != EXT2_OK) return -1;

        return (int32_t)(g * g_fs.blocks_per_group + (uint32_t)bit);
    }

    return -1;  /* No free blocks found. */
}

ext2_status_t ext2_free_block(uint32_t block_num) {
    if (!g_fs.mounted) return EXT2_ERR_NOT_MOUNTED;

    uint32_t group  = block_num / g_fs.blocks_per_group;
    uint32_t bit    = block_num % g_fs.blocks_per_group;
    if (group >= g_fs.num_groups) return EXT2_ERR_INVALID;

    uint32_t bitmap_block = g_fs.gd[group].bg_block_bitmap;

    uint8_t buf[1024];
    ext2_status_t st = ext2_read_block(bitmap_block, buf);
    if (st != EXT2_OK) return st;

    if (!bitmap_test(buf, bit)) return EXT2_ERR_INVALID;  /* Already free. */

    bitmap_clear(buf, bit);
    st = ext2_write_block(bitmap_block, buf);
    if (st != EXT2_OK) return st;

    return ext2_update_counts(group, 1, 0);
}

int32_t ext2_alloc_inode(void) {
    if (!g_fs.mounted) return -1;

    for (uint32_t g = 0; g < g_fs.num_groups; g++) {
        uint32_t bitmap_block = g_fs.gd[g].bg_inode_bitmap;

        uint8_t buf[1024];
        ext2_status_t st = ext2_read_block(bitmap_block, buf);
        if (st != EXT2_OK) return -1;

        int32_t bit = bitmap_find_free(buf, g_fs.inodes_per_group);
        if (bit < 0) continue;

        bitmap_set(buf, (uint32_t)bit);
        st = ext2_write_block(bitmap_block, buf);
        if (st != EXT2_OK) return -1;

        st = ext2_update_counts(g, 0, -1);
        if (st != EXT2_OK) return -1;

        /* Inode numbers are 1-based. bit 0 = first inode in group. */
        return (int32_t)(g * g_fs.inodes_per_group + (uint32_t)bit + 1);
    }

    return -1;
}

ext2_status_t ext2_free_inode(uint32_t ino) {
    if (!g_fs.mounted) return EXT2_ERR_NOT_MOUNTED;
    if (ino == 0) return EXT2_ERR_INVALID;

    uint32_t group = EXT2_INO_GROUP(ino, g_fs.inodes_per_group);
    uint32_t bit   = (ino - 1) % g_fs.inodes_per_group;
    if (group >= g_fs.num_groups) return EXT2_ERR_INVALID;

    uint32_t bitmap_block = g_fs.gd[group].bg_inode_bitmap;

    uint8_t buf[1024];
    ext2_status_t st = ext2_read_block(bitmap_block, buf);
    if (st != EXT2_OK) return st;

    if (!bitmap_test(buf, bit)) return EXT2_ERR_INVALID;  /* Already free. */

    bitmap_clear(buf, bit);
    st = ext2_write_block(bitmap_block, buf);
    if (st != EXT2_OK) return st;

    return ext2_update_counts(group, 0, 1);
}

/* =========================================================================
 * Root Directory Initialization
 * ========================================================================= */

ext2_status_t ext2_init_root_dir(void) {
    if (!g_fs.mounted) return EXT2_ERR_NOT_MOUNTED;

    uint32_t root_ino = EXT2_ROOT_INO;
    uint32_t group    = EXT2_INO_GROUP(root_ino, g_fs.inodes_per_group);

    /* --- Step 1: Allocate a data block for the directory entries. */
    int32_t root_data_block = ext2_alloc_block();
    if (root_data_block < 0) return EXT2_ERR_NO_SPACE;

    /* --- Step 2: Set up the root inode. */
    ext2_inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.i_mode        = EXT2_S_IFDIR | 0755;
    root_inode.i_links_count = 2;
    root_inode.i_blocks      = g_fs.block_size / 512;
    root_inode.i_size        = g_fs.block_size;
    root_inode.i_block[0]    = (uint32_t)root_data_block;

    /* Write the root inode (inode 2) to disk. */
    ext2_status_t st = ext2_write_inode(root_ino, &root_inode);
    if (st != EXT2_OK) return st;

    /* --- Step 3: Write directory entries into the data block. */
    uint8_t dir_buf[1024];
    memset(dir_buf, 0, g_fs.block_size);

    /* Entry 1: "." → inode 2, rec_len = 12 (8 + 1 name + 3 pad). */
    ext2_dirent_t *de = (ext2_dirent_t *)dir_buf;
    de->inode     = 2;
    de->rec_len   = 12;
    de->name_len  = 1;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';

    /* Entry 2: ".." → inode 2, rec_len fills rest of block. */
    de = (ext2_dirent_t *)(dir_buf + 12);
    de->inode     = 2;
    de->rec_len   = g_fs.block_size - 12;
    de->name_len  = 2;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';
    de->name[1]   = '.';

    st = ext2_write_block((uint32_t)root_data_block, dir_buf);
    if (st != EXT2_OK) return st;

    /* --- Step 4: Mark root inode as used in the bitmap.
     * The bitmap already has inodes 1-8 marked used in a fresh image.
     * For a clean init from a formatted image, we mark inode 2 as well.
     * ext2_alloc_inode would skip it only if bit 1 is set — which it
     * already is (byte 0 = 0xFF marks bits 0-7). So we just increment
     * bg_used_dirs_count. */
    g_fs.gd[group].bg_used_dirs_count++;

    /* Write updated group descriptor back to disk. */
    st = ext2_write_block(2, g_fs.gd);
    if (st != EXT2_OK) return st;

    ext2_log("[EXT2] Root directory initialized (inode 2).\r\n");
    return EXT2_OK;
}

/* =========================================================================
 * Directory Operations (Phase 6)
 * ========================================================================= */

uint32_t ext2_dir_lookup(uint32_t dir_ino, const char *name) {
    if (!g_fs.mounted || !name) return 0;

    ext2_inode_t dinode;
    if (ext2_read_inode(dir_ino, &dinode) != EXT2_OK) return 0;
    if (!(dinode.i_mode & EXT2_S_IFDIR)) return 0;

    uint32_t nlen = strlen(name);
    uint32_t nblocks = (dinode.i_size + g_fs.block_size - 1) / g_fs.block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_get_block(&dinode, b);
        if (phys == 0) continue;

        uint8_t block_buf[1024];
        if (ext2_read_block(phys, block_buf) != EXT2_OK) continue;

        uint32_t off = 0;
        while (off < g_fs.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + off);
            if (de->rec_len == 0 || de->rec_len > g_fs.block_size - off)
                break;
            if (de->inode != 0 &&
                de->name_len == (uint8_t)nlen &&
                memcmp(de->name, name, nlen) == 0) {
                return de->inode;
            }
            off += de->rec_len;
        }
    }
    return 0;
}

ext2_status_t ext2_dir_add_entry(uint32_t dir_ino, const char *name,
                                  uint32_t child_ino, uint8_t file_type) {
    if (!g_fs.mounted || !name) { ext2_log("[EXT2] dir_add_entry: invalid args\r\n"); return EXT2_ERR_INVALID; }

    ext2_log("[EXT2] dir_add_entry: dir_ino="); ext2_log_hex32(dir_ino);
    ext2_log(" name="); ext2_log(name);
    ext2_log(" child_ino="); ext2_log_hex32(child_ino);
    ext2_log(" type="); ext2_log_hex32(file_type);
    ext2_log("\r\n");

    ext2_inode_t dinode;
    if (ext2_read_inode(dir_ino, &dinode) != EXT2_OK) {
        ext2_log("[EXT2] dir_add_entry: read_inode FAILED\r\n");
        return EXT2_ERR_NOT_FOUND;
    }
    if (!(dinode.i_mode & EXT2_S_IFDIR)) {
        ext2_log("[EXT2] dir_add_entry: not a directory\r\n");
        return EXT2_ERR_INVALID;
    }

    ext2_log("[EXT2] dir_add_entry: dir_size="); ext2_log_hex32(dinode.i_size); ext2_log("\r\n");

    uint32_t nlen   = strlen(name);
    uint32_t needed  = (8 + nlen + 3) & ~3;
    if (needed < 12) needed = 12;

    uint32_t nblocks = (dinode.i_size + g_fs.block_size - 1) / g_fs.block_size;
    ext2_log("[EXT2] dir_add_entry: nblocks="); ext2_log_hex32(nblocks); ext2_log(" needed="); ext2_log_hex32(needed); ext2_log("\r\n");

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_get_block(&dinode, b);
        ext2_log("[EXT2] dir_add_entry: scanning block "); ext2_log_hex32(b); ext2_log(" phys="); ext2_log_hex32(phys); ext2_log("\r\n");
        if (phys == 0) continue;

        uint8_t block_buf[1024];
        if (ext2_read_block(phys, block_buf) != EXT2_OK) continue;

        uint32_t off = 0;
        ext2_dirent_t *prev = NULL;
        while (off < g_fs.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + off);
            if (de->rec_len == 0 || de->rec_len > g_fs.block_size - off)
                break;
            prev = de;
            off += de->rec_len;
        }
        if (prev) {
            uint32_t end_of_prev  = (uint32_t)((uint8_t *)prev - block_buf) + prev->rec_len;
            uint32_t slack        = g_fs.block_size - end_of_prev;
            ext2_log("[EXT2] dir_add_entry: prev rec_len="); ext2_log_hex32(prev->rec_len);
            ext2_log(" end="); ext2_log_hex32(end_of_prev);
            ext2_log(" slack="); ext2_log_hex32(slack); ext2_log("\r\n");
            if (slack >= needed) {
                uint32_t new_entry_off = end_of_prev;
                prev->rec_len = (uint16_t)(new_entry_off - (uint32_t)((uint8_t *)prev - block_buf));

                ext2_dirent_t *ne = (ext2_dirent_t *)(block_buf + new_entry_off);
                ne->inode     = child_ino;
                ne->rec_len   = (uint16_t)(g_fs.block_size - new_entry_off);
                ne->name_len  = (uint8_t)nlen;
                ne->file_type = file_type;
                memcpy(ne->name, name, nlen);

                ext2_log("[EXT2] dir_add_entry: added in existing block\r\n");
                ext2_status_t st = ext2_write_block(phys, block_buf);
                return st;
            }
        }
    }

    ext2_log("[EXT2] dir_add_entry: pass 2 — allocating new block\r\n");
    uint32_t logical = nblocks;
    ext2_status_t st = ext2_inode_alloc_block(dir_ino, &dinode, logical);
    if (st != EXT2_OK) { ext2_log("[EXT2] dir_add_entry: alloc_block FAILED\r\n"); return st; }

    dinode.i_size = (logical + 1) * g_fs.block_size;
    ext2_write_inode(dir_ino, &dinode);

    uint32_t new_phys = ext2_inode_get_block(&dinode, logical);
    if (new_phys == 0) { ext2_log("[EXT2] dir_add_entry: get_block FAILED\r\n"); return EXT2_ERR_IO; }

    ext2_log("[EXT2] dir_add_entry: new phys="); ext2_log_hex32(new_phys); ext2_log("\r\n");

    uint8_t new_block[1024];
    memset(new_block, 0, g_fs.block_size);

    ext2_dirent_t *ne = (ext2_dirent_t *)new_block;
    ne->inode     = child_ino;
    ne->rec_len   = (uint16_t)g_fs.block_size;
    ne->name_len  = (uint8_t)nlen;
    ne->file_type = file_type;
    memcpy(ne->name, name, nlen);

    st = ext2_write_block(new_phys, new_block);
    ext2_log("[EXT2] dir_add_entry: DONE (new block), st="); ext2_log_hex32((uint32_t)st); ext2_log("\r\n");
    return st;
}

ext2_status_t ext2_dir_remove_entry(uint32_t dir_ino, const char *name) {
    if (!g_fs.mounted || !name) return EXT2_ERR_INVALID;

    ext2_inode_t dinode;
    if (ext2_read_inode(dir_ino, &dinode) != EXT2_OK)
        return EXT2_ERR_NOT_FOUND;
    if (!(dinode.i_mode & EXT2_S_IFDIR))
        return EXT2_ERR_INVALID;

    uint32_t nlen    = strlen(name);
    uint32_t nblocks = (dinode.i_size + g_fs.block_size - 1) / g_fs.block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_get_block(&dinode, b);
        if (phys == 0) continue;

        uint8_t block_buf[1024];
        if (ext2_read_block(phys, block_buf) != EXT2_OK) continue;

        uint32_t off = 0;
        while (off < g_fs.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + off);
            if (de->rec_len == 0 || de->rec_len > g_fs.block_size - off)
                break;
            if (de->inode != 0 &&
                de->name_len == (uint8_t)nlen &&
                memcmp(de->name, name, nlen) == 0) {
                de->inode = 0;
                return ext2_write_block(phys, block_buf);
            }
            off += de->rec_len;
        }
    }
    return EXT2_ERR_NOT_FOUND;
}

uint32_t ext2_mkdir(uint32_t parent_ino, const char *name) {
    if (!g_fs.mounted || !name) {
        ext2_log("[EXT2] mkdir: not mounted or null name\r\n");
        return 0;
    }

    ext2_log("[EXT2] mkdir: parent_ino="); ext2_log_hex32(parent_ino); ext2_log(" name="); ext2_log(name); ext2_log("\r\n");

    int32_t new_ino = ext2_alloc_inode();
    if (new_ino < 0) { ext2_log("[EXT2] mkdir: alloc_inode FAILED\r\n"); return 0; }
    ext2_log("[EXT2] mkdir: alloc_inode -> "); ext2_log_hex32((uint32_t)new_ino); ext2_log("\r\n");

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = EXT2_S_IFDIR | 0755;
    inode.i_links_count = 2;
    ext2_status_t st = ext2_write_inode((uint32_t)new_ino, &inode);
    if (st != EXT2_OK) { ext2_log("[EXT2] mkdir: write_inode FAILED\r\n"); return 0; }

    st = ext2_inode_alloc_block((uint32_t)new_ino, &inode, 0);
    if (st != EXT2_OK) { ext2_log("[EXT2] mkdir: alloc_block FAILED\r\n"); return 0; }

    inode.i_size = g_fs.block_size;
    ext2_write_inode((uint32_t)new_ino, &inode);

    uint32_t phys = ext2_inode_get_block(&inode, 0);
    if (phys == 0) { ext2_log("[EXT2] mkdir: get_block FAILED\r\n"); return 0; }
    ext2_log("[EXT2] mkdir: dir phys_block="); ext2_log_hex32(phys); ext2_log("\r\n");

    uint8_t block_buf[1024];
    memset(block_buf, 0, g_fs.block_size);

    ext2_dirent_t *de = (ext2_dirent_t *)block_buf;
    de->inode     = (uint32_t)new_ino;
    de->rec_len   = 12;
    de->name_len  = 1;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';

    de = (ext2_dirent_t *)(block_buf + 12);
    de->inode     = parent_ino;
    de->rec_len   = (uint16_t)(g_fs.block_size - 12);
    de->name_len  = 2;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';
    de->name[1]   = '.';

    st = ext2_write_block(phys, block_buf);
    if (st != EXT2_OK) { ext2_log("[EXT2] mkdir: write_block FAILED\r\n"); return 0; }

    st = ext2_dir_add_entry(parent_ino, name, (uint32_t)new_ino, EXT2_FT_DIR);
    if (st != EXT2_OK) { ext2_log("[EXT2] mkdir: dir_add_entry FAILED\r\n"); return 0; }

    ext2_inode_t pinode;
    st = ext2_read_inode(parent_ino, &pinode);
    if (st != EXT2_OK) { ext2_log("[EXT2] mkdir: read_parent FAILED\n"); return (uint32_t)new_ino; }
    pinode.i_links_count++;
    ext2_write_inode(parent_ino, &pinode);

    uint32_t group = EXT2_INO_GROUP((uint32_t)new_ino, g_fs.inodes_per_group);
    g_fs.gd[group].bg_used_dirs_count++;
    ext2_write_block(2, g_fs.gd);

    ext2_log("[EXT2] mkdir: DONE new_ino="); ext2_log_hex32((uint32_t)new_ino); ext2_log("\r\n");
    return (uint32_t)new_ino;
}

/* =========================================================================
 * File Metadata, Unlink, Rmdir, Getdents, Rename, Truncate (Phase 9)
 * ========================================================================= */

ext2_status_t ext2_stat(uint32_t ino, ext2_stat_t *st) {
    if (!g_fs.mounted || !st) return EXT2_ERR_INVALID;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != EXT2_OK) return EXT2_ERR_NOT_FOUND;

    st->ino    = ino;
    st->size   = inode.i_size;
    st->mode   = inode.i_mode;
    st->links  = inode.i_links_count;
    st->blocks = inode.i_blocks;
    st->uid    = inode.i_uid;
    st->gid    = inode.i_gid;
    st->atime  = inode.i_atime;
    st->mtime  = inode.i_mtime;
    st->ctime  = inode.i_ctime;
    return EXT2_OK;
}

ext2_status_t ext2_free_all_blocks(uint32_t ino, ext2_inode_t *inode) {
    if (!inode) return EXT2_ERR_INVALID;
    (void)ino;

    uint32_t ptrs = EXT2_PTRS_PER_BLOCK;

    /* Free direct blocks [0..11]. */
    for (int i = 0; i < EXT2_N_DIRECT; i++) {
        if (inode->i_block[i] != 0) {
            ext2_free_block(inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }

    /* Free single indirect [12]. */
    if (inode->i_block[12] != 0) {
        uint32_t ind[256];
        if (ext2_read_block(inode->i_block[12], ind) == EXT2_OK) {
            for (uint32_t i = 0; i < ptrs; i++) {
                if (ind[i] != 0) ext2_free_block(ind[i]);
            }
        }
        ext2_free_block(inode->i_block[12]);
        inode->i_block[12] = 0;
    }

    /* Free double indirect [13]. */
    if (inode->i_block[13] != 0) {
        uint32_t L1[256];
        if (ext2_read_block(inode->i_block[13], L1) == EXT2_OK) {
            for (uint32_t i = 0; i < ptrs; i++) {
                if (L1[i] != 0) {
                    uint32_t L0[256];
                    if (ext2_read_block(L1[i], L0) == EXT2_OK) {
                        for (uint32_t j = 0; j < ptrs; j++) {
                            if (L0[j] != 0) ext2_free_block(L0[j]);
                        }
                    }
                    ext2_free_block(L1[i]);
                }
            }
        }
        ext2_free_block(inode->i_block[13]);
        inode->i_block[13] = 0;
    }

    /* Free triple indirect [14]. */
    if (inode->i_block[14] != 0) {
        uint32_t L2[256];
        if (ext2_read_block(inode->i_block[14], L2) == EXT2_OK) {
            for (uint32_t i = 0; i < ptrs; i++) {
                if (L2[i] != 0) {
                    uint32_t L1[256];
                    if (ext2_read_block(L2[i], L1) == EXT2_OK) {
                        for (uint32_t j = 0; j < ptrs; j++) {
                            if (L1[j] != 0) {
                                uint32_t L0[256];
                                if (ext2_read_block(L1[j], L0) == EXT2_OK) {
                                    for (uint32_t k = 0; k < ptrs; k++) {
                                        if (L0[k] != 0) ext2_free_block(L0[k]);
                                    }
                                }
                                ext2_free_block(L1[j]);
                            }
                        }
                    }
                    ext2_free_block(L2[i]);
                }
            }
        }
        ext2_free_block(inode->i_block[14]);
        inode->i_block[14] = 0;
    }

    inode->i_blocks = 0;
    return EXT2_OK;
}

ext2_status_t ext2_truncate(uint32_t ino, uint64_t new_size) {
    if (!g_fs.mounted) return EXT2_ERR_NOT_MOUNTED;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != EXT2_OK) return EXT2_ERR_NOT_FOUND;

    uint32_t old_size = inode.i_size;

    if (new_size >= (uint64_t)old_size) {
        /* Extending: just update size (zero-fill handled by read sparse). */
        inode.i_size = (uint32_t)new_size;
        return ext2_write_inode(ino, &inode);
    }

    /* Shrinking. */
    uint32_t old_blocks = (old_size + g_fs.block_size - 1) / g_fs.block_size;
    uint32_t new_blocks = (new_size == 0) ? 0 :
                          ((new_size + g_fs.block_size - 1) / g_fs.block_size);

    /* Free blocks past new_blocks. */
    for (uint32_t b = new_blocks; b < old_blocks; b++) {
        uint32_t phys = ext2_inode_get_block(&inode, b);
        if (phys != 0) {
            ext2_free_block(phys);
            inode.i_block[b < EXT2_N_DIRECT ? b : EXT2_N_DIRECT] = 0;
        }
    }

    /* Zero tail of last partial block if needed. */
    if (new_size > 0 && new_size % g_fs.block_size != 0) {
        uint32_t last_phys = ext2_inode_get_block(&inode, new_blocks - 1);
        if (last_phys != 0) {
            uint8_t blk_buf[1024];
            if (ext2_read_block(last_phys, blk_buf) == EXT2_OK) {
                uint32_t tail = new_size % g_fs.block_size;
                memset(blk_buf + tail, 0, g_fs.block_size - tail);
                ext2_write_block(last_phys, blk_buf);
            }
        }
    }

    inode.i_size = (uint32_t)new_size;
    return ext2_write_inode(ino, &inode);
}

ext2_status_t ext2_unlink(uint32_t parent_ino, const char *name) {
    if (!g_fs.mounted || !name) return EXT2_ERR_INVALID;

    /* Look up the child. */
    uint32_t child_ino = ext2_dir_lookup(parent_ino, name);
    if (child_ino == 0) return EXT2_ERR_NOT_FOUND;

    /* Read child inode. */
    ext2_inode_t child;
    if (ext2_read_inode(child_ino, &child) != EXT2_OK)
        return EXT2_ERR_NOT_FOUND;

    /* Don't allow unlinking directories (use rmdir). */
    if (EXT2_S_ISDIR(child.i_mode)) return EXT2_ERR_INVALID;

    /* Remove the directory entry. */
    ext2_status_t st = ext2_dir_remove_entry(parent_ino, name);
    if (st != EXT2_OK) return st;

    /* Decrement link count. */
    child.i_links_count--;

    if (child.i_links_count == 0) {
        /* Free all data blocks. */
        ext2_free_all_blocks(child_ino, &child);

        /* Free the inode. */
        ext2_free_inode(child_ino);
    } else {
        /* Write updated inode back. */
        ext2_write_inode(child_ino, &child);
    }

    return EXT2_OK;
}

ext2_status_t ext2_rmdir(uint32_t parent_ino, const char *name) {
    if (!g_fs.mounted || !name) return EXT2_ERR_INVALID;

    uint32_t child_ino = ext2_dir_lookup(parent_ino, name);
    if (child_ino == 0) return EXT2_ERR_NOT_FOUND;

    ext2_inode_t child;
    if (ext2_read_inode(child_ino, &child) != EXT2_OK)
        return EXT2_ERR_NOT_FOUND;

    /* Must be a directory. */
    if (!EXT2_S_ISDIR(child.i_mode)) return EXT2_ERR_INVALID;

    /* Must be empty (only "." and ".." entries). */
    uint32_t nblocks = (child.i_size + g_fs.block_size - 1) / g_fs.block_size;
    uint32_t entry_count = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_get_block(&child, b);
        if (phys == 0) continue;
        uint8_t block_buf[1024];
        if (ext2_read_block(phys, block_buf) != EXT2_OK) continue;
        uint32_t off = 0;
        while (off < g_fs.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + off);
            if (de->rec_len == 0 || de->rec_len > g_fs.block_size - off) break;
            if (de->inode != 0) entry_count++;
            off += de->rec_len;
        }
    }
    if (entry_count > 2) return EXT2_ERR_INVALID; /* not empty */

    /* Remove from parent. */
    ext2_status_t st = ext2_dir_remove_entry(parent_ino, name);
    if (st != EXT2_OK) return st;

    /* Decrement parent's link count (for ".."). */
    ext2_inode_t parent;
    if (ext2_read_inode(parent_ino, &parent) == EXT2_OK) {
        parent.i_links_count--;
        ext2_write_inode(parent_ino, &parent);
    }

    /* Decrement group used_dirs_count. */
    uint32_t group = EXT2_INO_GROUP(child_ino, g_fs.inodes_per_group);
    if (g_fs.gd[group].bg_used_dirs_count > 0) {
        g_fs.gd[group].bg_used_dirs_count--;
        ext2_write_block(2, g_fs.gd);
    }

    /* Free child's data blocks and inode. */
    ext2_free_all_blocks(child_ino, &child);
    ext2_free_inode(child_ino);

    return EXT2_OK;
}

int32_t ext2_getdents(uint32_t dir_ino, uint64_t *cookie,
                       void *buf, uint32_t count) {
    if (!g_fs.mounted || !cookie || !buf || count == 0) return -1;

    ext2_inode_t dinode;
    if (ext2_read_inode(dir_ino, &dinode) != EXT2_OK) return -1;
    if (!EXT2_S_ISDIR(dinode.i_mode)) return -1;

    uint32_t nblocks = (dinode.i_size + g_fs.block_size - 1) / g_fs.block_size;
    uint32_t bytes_written = 0;
    uint32_t buf_pos = 0;
    uint64_t skip = *cookie;

    for (uint32_t b = 0; b < nblocks && buf_pos < count; b++) {
        uint32_t phys = ext2_inode_get_block(&dinode, b);
        if (phys == 0) continue;

        uint8_t block_buf[1024];
        if (ext2_read_block(phys, block_buf) != EXT2_OK) continue;

        uint32_t block_bytes = (uint32_t)b * g_fs.block_size;
        uint32_t off = 0;
        while (off < g_fs.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + off);
            if (de->rec_len == 0 || de->rec_len > g_fs.block_size - off) break;

            uint64_t entry_offset = (uint64_t)block_bytes + off;
            off += de->rec_len;

            if (de->inode == 0) continue;
            if (skip > 0) { skip--; continue; }

            uint16_t name_len = de->name_len;
            uint16_t reclen = sizeof(ext2_dirent64_t) + name_len;
            reclen = (reclen + 7) & ~7; /* 8-byte align */

            if (buf_pos + reclen > count) goto done;

            ext2_dirent64_t *out = (ext2_dirent64_t *)((uint8_t *)buf + buf_pos);
            out->d_ino    = de->inode;
            out->d_off    = entry_offset + de->rec_len;
            out->d_reclen = reclen;
            out->d_type   = de->file_type;
            memcpy(out->d_name, de->name, name_len);
            out->d_name[name_len] = '\0';

            buf_pos += reclen;
            bytes_written += reclen;
        }
    }

done:
    *cookie += buf_pos > 0 ? (bytes_written > 0 ? buf_pos : 0) : 0;
    /* Actually, advance cookie by the number of entries we skipped + wrote. */
    /* Cookie should reflect byte position in directory, so we just track
     * how many bytes into the dir we've scanned. Simplest: just return
     * bytes_written and let the caller call again. */
    (void)bytes_written;
    return (int32_t)buf_pos;
}

ext2_status_t ext2_rename(uint32_t old_dir, const char *old_name,
                           uint32_t new_dir, const char *new_name) {
    if (!g_fs.mounted || !old_name || !new_name) return EXT2_ERR_INVALID;

    /* Look up the child. */
    uint32_t child_ino = ext2_dir_lookup(old_dir, old_name);
    if (child_ino == 0) return EXT2_ERR_NOT_FOUND;

    /* Remove from old parent. */
    ext2_status_t st = ext2_dir_remove_entry(old_dir, old_name);
    if (st != EXT2_OK) return st;

    /* Determine file type. */
    ext2_inode_t child;
    uint8_t ft = EXT2_FT_REG_FILE;
    if (ext2_read_inode(child_ino, &child) == EXT2_OK) {
        if (EXT2_S_ISDIR(child.i_mode)) ft = EXT2_FT_DIR;
    }

    /* Add to new parent. */
    st = ext2_dir_add_entry(new_dir, new_name, child_ino, ft);
    if (st != EXT2_OK) return st;

    return EXT2_OK;
}

/* =========================================================================
 * Path Resolution (Phase 7)
 * ========================================================================= */

void ext2_split_path(const char *path, char *parent, char *name) {
    if (!path || !parent || !name) {
        if (parent) parent[0] = '\0';
        if (name)   name[0]   = '\0';
        return;
    }

    /* Find the last '/' by scanning backward. */
    int last_slash = -1;
    int len = 0;
    for (int i = 0; path[i]; i++) {
        len = i;
        if (path[i] == '/') last_slash = i;
    }
    (void)len;

    if (last_slash < 0) {
        /* No slash: parent = "/", name = entire path. */
        parent[0] = '/';
        parent[1] = '\0';
        int j = 0;
        while (path[j]) { name[j] = path[j]; j++; }
        name[j] = '\0';
    } else if (last_slash == 0) {
        /* Slash at position 0: parent = "/", name = after slash. */
        parent[0] = '/';
        parent[1] = '\0';
        int j = 0;
        while (path[1 + j]) { name[j] = path[1 + j]; j++; }
        name[j] = '\0';
    } else {
        /* Slash in middle: parent = [0..last_slash-1], name = [last_slash+1..] */
        int j = 0;
        while (j < last_slash) { parent[j] = path[j]; j++; }
        parent[j] = '\0';
        j = 0;
        while (path[last_slash + 1 + j]) { name[j] = path[last_slash + 1 + j]; j++; }
        name[j] = '\0';
    }
}

uint32_t ext2_resolve_path(const char *path) {
    if (!g_fs.mounted || !path) { ext2_log("[EXT2] resolve_path: not mounted or null\r\n"); return 0; }

    uint32_t current = EXT2_ROOT_INO;

    /* Skip leading slashes. */
    while (*path == '/') path++;

    /* Empty path or just "/" → root. */
    if (*path == '\0') {
        ext2_log("[EXT2] resolve_path: '/' -> ino=2\r\n");
        return EXT2_ROOT_INO;
    }

    ext2_log("[EXT2] resolve_path: "); ext2_log(path); ext2_log(" -> ");

    /* Process each component. */
    while (*path) {
        /* Extract component name (up to next '/'). */
        char comp[EXT2_MAX_PATH];
        int ci = 0;
        while (path[ci] && path[ci] != '/' && ci < EXT2_MAX_PATH - 1) {
            comp[ci] = path[ci];
            ci++;
        }
        comp[ci] = '\0';

        if (ci == 0) {
            /* Empty component (double slash). */
            path++;
            continue;
        }

        uint32_t found = ext2_dir_lookup(current, comp);
        if (found == 0) return 0;
        current = found;

        path += ci;
        /* Skip separator. */
        if (*path == '/') path++;
    }

    ext2_log("ino="); ext2_log_hex32(current); ext2_log("\r\n");
    return current;
}

/* =========================================================================
 * File Descriptor Integration (Phase 8)
 * ========================================================================= */

static int64_t ext2_fd_read(int fd, void *buf, uint64_t count, void *data) {
    (void)fd;
    ext2_file_t *file = (ext2_file_t *)data;
    if (!file) return -1;
    int64_t n = ext2_read_file(file->ino, buf, file->offset, count);
    if (n > 0) file->offset += (uint64_t)n;
    return n;
}

static int64_t ext2_fd_write(int fd, const void *buf, uint64_t count, void *data) {
    (void)fd;
    ext2_file_t *file = (ext2_file_t *)data;
    if (!file) return -1;
    if (file->flags == O_RDONLY) return -1;
    int64_t n = ext2_write_file(file->ino, buf, file->offset, count);
    if (n > 0) file->offset += (uint64_t)n;
    return n;
}

static void ext2_fd_close(int fd, void *data) {
    (void)fd;
    kfree(data);
}

file_ops_t ext2_file_ops = {
    .read  = ext2_fd_read,
    .write = ext2_fd_write,
    .close = ext2_fd_close,
};

/* =========================================================================
 * VFS Adapter — path-based operations for vfs_fs_ops_t
 *
 * All paths received here are mount-relative (leading "/" stripped if
 * mount is "/", but for root mount "/" the rel_path is "" or "/" for root dir).
 * ========================================================================= */

static int ext2_vfs_open(const char *rel_path, uint32_t flags,
                          file_ops_t **ops, void **data) {
    /* Build absolute path for ext2_resolve_path (it expects leading '/'). */
    char full[EXT2_MAX_PATH];
    if (rel_path[0] == '/') {
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 1) { full[i] = rel_path[i]; i++; }
        full[i] = '\0';
    } else {
        full[0] = '/';
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 2) { full[i + 1] = rel_path[i]; i++; }
        full[i + 1] = '\0';
    }

    uint32_t ino = ext2_resolve_path(full);
    ext2_log("[EXT2] vfs_open: path="); ext2_log(full);
    ext2_log(" flags="); ext2_log_hex32(flags);
    ext2_log(" ino="); ext2_log_hex32(ino);
    ext2_log("\r\n");

    if (ino == 0 && (flags & O_CREAT)) {
        char parent[EXT2_MAX_PATH];
        char name[EXT2_MAX_PATH];
        ext2_split_path(full, parent, name);
        if (name[0] == '\0') return -1;

        uint32_t parent_ino = ext2_resolve_path(parent);
        if (parent_ino == 0) return -1;

        int32_t new_ino = ext2_alloc_inode();
        if (new_ino < 0) return -1;

        ext2_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.i_mode        = EXT2_S_IFREG | 0644;
        inode.i_links_count = 1;
        ext2_write_inode((uint32_t)new_ino, &inode);

        ext2_status_t st = ext2_dir_add_entry(parent_ino, name,
                                               (uint32_t)new_ino,
                                               EXT2_FT_REG_FILE);
        if (st != EXT2_OK) return -1;
        ino = (uint32_t)new_ino;
    }

    if (ino == 0) return -1;

    ext2_file_t *file = (ext2_file_t *)kmalloc(sizeof(ext2_file_t));
    if (!file) return -1;
    file->ino    = ino;
    file->offset = 0;
    file->flags  = flags;

    *ops  = &ext2_file_ops;
    *data = file;
    return 0;
}

static int ext2_vfs_stat(const char *rel_path, ext2_stat_t *st) {
    char full[EXT2_MAX_PATH];
    if (rel_path[0] == '/') {
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 1) { full[i] = rel_path[i]; i++; }
        full[i] = '\0';
    } else {
        full[0] = '/';
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 2) { full[i + 1] = rel_path[i]; i++; }
        full[i + 1] = '\0';
    }

    uint32_t ino = ext2_resolve_path(full);
    if (ino == 0) return -1;
    return (ext2_stat(ino, st) == EXT2_OK) ? 0 : -1;
}

static int ext2_vfs_unlink(const char *rel_path) {
    char full[EXT2_MAX_PATH];
    if (rel_path[0] == '/') {
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 1) { full[i] = rel_path[i]; i++; }
        full[i] = '\0';
    } else {
        full[0] = '/';
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 2) { full[i + 1] = rel_path[i]; i++; }
        full[i + 1] = '\0';
    }

    char parent[EXT2_MAX_PATH], name[EXT2_MAX_PATH];
    ext2_split_path(full, parent, name);
    if (name[0] == '\0') return -1;

    uint32_t parent_ino = ext2_resolve_path(parent);
    if (parent_ino == 0) return -1;

    return (ext2_unlink(parent_ino, name) == EXT2_OK) ? 0 : -1;
}

static int ext2_vfs_rmdir(const char *rel_path) {
    char full[EXT2_MAX_PATH];
    if (rel_path[0] == '/') {
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 1) { full[i] = rel_path[i]; i++; }
        full[i] = '\0';
    } else {
        full[0] = '/';
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 2) { full[i + 1] = rel_path[i]; i++; }
        full[i + 1] = '\0';
    }

    char parent[EXT2_MAX_PATH], name[EXT2_MAX_PATH];
    ext2_split_path(full, parent, name);
    if (name[0] == '\0') return -1;

    uint32_t parent_ino = ext2_resolve_path(parent);
    if (parent_ino == 0) return -1;

    return (ext2_rmdir(parent_ino, name) == EXT2_OK) ? 0 : -1;
}

static int ext2_vfs_rename(const char *old_rel, const char *new_rel) {
    char old_full[EXT2_MAX_PATH], new_full[EXT2_MAX_PATH];

    /* Build absolute paths. */
    if (old_rel[0] == '/') {
        int i = 0;
        while (old_rel[i] && i < EXT2_MAX_PATH - 1) { old_full[i] = old_rel[i]; i++; }
        old_full[i] = '\0';
    } else {
        old_full[0] = '/';
        int i = 0;
        while (old_rel[i] && i < EXT2_MAX_PATH - 2) { old_full[i + 1] = old_rel[i]; i++; }
        old_full[i + 1] = '\0';
    }

    if (new_rel[0] == '/') {
        int i = 0;
        while (new_rel[i] && i < EXT2_MAX_PATH - 1) { new_full[i] = new_rel[i]; i++; }
        new_full[i] = '\0';
    } else {
        new_full[0] = '/';
        int i = 0;
        while (new_rel[i] && i < EXT2_MAX_PATH - 2) { new_full[i + 1] = new_rel[i]; i++; }
        new_full[i + 1] = '\0';
    }

    char old_parent[EXT2_MAX_PATH], old_name[EXT2_MAX_PATH];
    char new_parent[EXT2_MAX_PATH], new_name[EXT2_MAX_PATH];

    ext2_split_path(old_full, old_parent, old_name);
    ext2_split_path(new_full, new_parent, new_name);

    if (old_name[0] == '\0' || new_name[0] == '\0') return -1;

    uint32_t old_parent_ino = ext2_resolve_path(old_parent);
    uint32_t new_parent_ino = ext2_resolve_path(new_parent);

    if (old_parent_ino == 0 || new_parent_ino == 0) return -1;

    return (ext2_rename(old_parent_ino, old_name,
                        new_parent_ino, new_name) == EXT2_OK) ? 0 : -1;
}

static int ext2_vfs_mkdir(const char *rel_path, uint32_t mode) {
    (void)mode;
    ext2_log("[EXT2] vfs_mkdir rel_path="); ext2_log(rel_path); ext2_log("\r\n");
    char full[EXT2_MAX_PATH];
    if (rel_path[0] == '/') {
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 1) { full[i] = rel_path[i]; i++; }
        full[i] = '\0';
    } else {
        full[0] = '/';
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 2) { full[i + 1] = rel_path[i]; i++; }
        full[i + 1] = '\0';
    }

    ext2_log("[EXT2] vfs_mkdir full="); ext2_log(full); ext2_log("\r\n");
    char parent[EXT2_MAX_PATH], name[EXT2_MAX_PATH];
    ext2_split_path(full, parent, name);
    ext2_log("[EXT2] vfs_mkdir parent="); ext2_log(parent); ext2_log(" name="); ext2_log(name); ext2_log("\r\n");
    if (name[0] == '\0') { ext2_log("[EXT2] vfs_mkdir: empty name\r\n"); return -1; }

    uint32_t parent_ino = ext2_resolve_path(parent);
    if (parent_ino == 0) { ext2_log("[EXT2] vfs_mkdir: parent not found\r\n"); return -1; }

    uint32_t ret = ext2_mkdir(parent_ino, name);
    ext2_log("[EXT2] vfs_mkdir: ret="); ext2_log_hex32(ret); ext2_log("\r\n");
    return (ret != 0) ? 0 : -1;
}

static int ext2_vfs_getdents(const char *rel_path, uint64_t *cookie,
                              void *buf, uint32_t count) {
    ext2_log("[EXT2] vfs_getdents rel_path="); ext2_log(rel_path); ext2_log("\r\n");
    char full[EXT2_MAX_PATH];
    if (rel_path[0] == '/') {
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 1) { full[i] = rel_path[i]; i++; }
        full[i] = '\0';
    } else {
        full[0] = '/';
        int i = 0;
        while (rel_path[i] && i < EXT2_MAX_PATH - 2) { full[i + 1] = rel_path[i]; i++; }
        full[i + 1] = '\0';
    }

    ext2_log("[EXT2] vfs_getdents full="); ext2_log(full); ext2_log("\r\n");
    uint32_t ino = ext2_resolve_path(full);
    if (ino == 0) { ext2_log("[EXT2] vfs_getdents: resolve FAILED\r\n"); return -1; }

    int ret = (int)ext2_getdents(ino, cookie, buf, count);
    ext2_log("[EXT2] vfs_getdents: ret="); ext2_log_hex32(ret); ext2_log(" cookie="); ext2_log_hex32((uint32_t)*cookie); ext2_log("\r\n");
    return ret;
}

vfs_fs_ops_t ext2_vfs_ops = {
    .name     = "ext2",
    .open     = ext2_vfs_open,
    .stat     = ext2_vfs_stat,
    .unlink  = ext2_vfs_unlink,
    .rmdir   = ext2_vfs_rmdir,
    .rename  = ext2_vfs_rename,
    .mkdir   = ext2_vfs_mkdir,
    .getdents = ext2_vfs_getdents,
};
