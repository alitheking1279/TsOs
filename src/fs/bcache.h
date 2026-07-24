/**
 * @file bcache.h
 * @brief Buffer Cache — block-level caching layer for the VFS.
 *
 * Provides in-memory caching of disk sectors to avoid redundant I/O.
 * Each cached block (buf_t) holds one sector of data and tracks its
 * dirty state, reference count, and position in the LRU eviction list.
 *
 * Design:
 *   - 128-buffer static pool (64 KiB total data footprint).
 *   - 16-bucket hash table for O(1) lookup by (dev_id, block_num).
 *   - Doubly-linked LRU list for eviction ordering.
 *   - Single global spinlock for thread safety.
 *   - Reference counting: get increments ref, put decrements.
 *     Eviction only considers buffers with ref == 0.
 *
 * Lifecycle:
 *   1. bcache_init()          — zero the pool, hash table, LRU list.
 *   2. bcache_get(dev, blk)   — find or allocate a buffer for (dev, blk).
 *   3. bcache_dirty(buf)       — mark the buffer as modified.
 *   4. bcache_put(buf)         — release the buffer (decrement ref).
 *   5. bcache_flush_dev(dev)   — write all dirty buffers for a device.
 *   6. bcache_invalidate(dev)  — discard all buffers for a device.
 */

#ifndef FS_BCACHE_H
#define FS_BCACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Number of buffers in the cache pool. */
#define BCACHE_NBUFS         128

/** Sector size in bytes (must match BLOCK_DEV_SECTOR_SIZE). */
#define BCACHE_SECTOR_SIZE   512

/** Number of hash buckets. */
#define BCACHE_HASH_SIZE     16

/* =========================================================================
 * Buffer Flags
 * ========================================================================= */

#define BCACHE_FLAG_VALID    (1 << 0)  /**< Buffer contains valid data. */
#define BCACHE_FLAG_DIRTY    (1 << 1)  /**< Buffer has been modified. */
#define BCACHE_FLAG_BUSY     (1 << 2)  /**< Buffer is locked (for future use). */

/* =========================================================================
 * Status Codes
 * ========================================================================= */

typedef enum {
    BCACHE_OK              =  0,
    BCACHE_ERR_NOT_FOUND   = -1,  /**< No buffer for (dev, blk) and pool is full. */
    BCACHE_ERR_IO          = -2,  /**< Underlying block device I/O error. */
    BCACHE_ERR_INVALID     = -3,  /**< NULL buffer or bad parameters. */
} bcache_status_t;

/* =========================================================================
 * Buffer Structure
 * ========================================================================= */

/**
 * @brief A single cached block.
 *
 * Embeds the sector data inline to avoid per-buffer dynamic allocation.
 * The data array is aligned to 512 bytes for DMA-safe I/O in the future.
 */
typedef struct buf {
    uint8_t  data[BCACHE_SECTOR_SIZE]; /**< Sector data (512 bytes). */

    uint8_t  dev_id;          /**< Block device ID this buffer belongs to. */
    uint32_t block_num;       /**< Sector number (LBA) on the device. */
    uint8_t  flags;           /**< BCACHE_FLAG_* bitmask. */
    uint32_t ref_count;       /**< Number of holders (get/put). */

    /* LRU doubly-linked list (for eviction ordering). */
    struct buf *lru_prev;
    struct buf *lru_next;

    /* Hash chain (for bucket collision resolution). */
    struct buf *hash_next;
} buf_t;

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct {
    uint32_t total_buffers;     /**< Total buffers in pool (BCACHE_NBUFS). */
    uint32_t free_buffers;      /**< Buffers with ref == 0 and not VALID. */
    uint32_t valid_buffers;     /**< Buffers with VALID flag set. */
    uint32_t dirty_buffers;     /**< Buffers with DIRTY flag set. */
    uint32_t hash_hits;         /**< Lookup found in hash table. */
    uint32_t hash_misses;       /**< Lookup required eviction/allocation. */
    uint32_t flushes;           /**< Number of flush operations. */
    uint32_t evictions;         /**< Number of buffers evicted. */
} bcache_stats_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the buffer cache.
 *
 * Zeros the pool, hash table, and LRU list.  Must be called before
 * any bcache_get() calls.
 *
 * @param serial_dev  Serial device for logging.  May be NULL.
 */
void bcache_init(void *serial_dev);

/**
 * @brief Get a buffer for the given device and block number.
 *
 * If the buffer is already cached, returns it (incrementing ref_count).
 * Otherwise, allocates a buffer from the pool (evicting LRU if needed)
 * and reads the sector from the block device.
 *
 * The caller MUST call bcache_put() when done with the buffer.
 *
 * @param dev_id    Block device ID.
 * @param block_num Sector number (LBA).
 * @return Pointer to the buffer, or NULL on I/O error or pool exhaustion.
 */
buf_t *bcache_get(uint8_t dev_id, uint32_t block_num);

/**
 * @brief Release a buffer (decrement reference count).
 *
 * If ref_count reaches zero, the buffer becomes a candidate for
 * eviction.  The buffer data is NOT written to disk by this call.
 *
 * @param buf  Buffer returned by bcache_get().  NULL is a no-op.
 */
void bcache_put(buf_t *buf);

/**
 * @brief Mark a buffer as dirty (modified).
 *
 * The buffer will be written to disk on the next bcache_flush_dev()
 * or when evicted.
 *
 * @param buf  Buffer returned by bcache_get().  NULL is a no-op.
 */
void bcache_dirty(buf_t *buf);

/**
 * @brief Flush all dirty buffers for a specific device.
 *
 * Writes each dirty buffer's data to the underlying block device.
 *
 * @param dev_id  Block device ID to flush.
 * @return BCACHE_OK on success, or the first I/O error encountered.
 */
bcache_status_t bcache_flush_dev(uint8_t dev_id);

/**
 * @brief Flush ALL dirty buffers across all devices.
 *
 * @return BCACHE_OK on success, or the first I/O error encountered.
 */
bcache_status_t bcache_flush_all(void);

/**
 * @brief Invalidate (discard) all buffers for a device.
 *
 * All buffers for the given device are removed from the hash table
 * and their VALID/DIRTY flags are cleared.  Dirty data is NOT written
 * to disk — the caller must call bcache_flush_dev() first if needed.
 *
 * @param dev_id  Block device ID to invalidate.
 */
void bcache_invalidate_dev(uint8_t dev_id);

/**
 * @brief Invalidate ALL buffers across all devices.
 */
void bcache_invalidate_all(void);

/**
 * @brief Fill a bcache_stats_t with current cache statistics.
 *
 * @param out  Destination struct.  Must not be NULL.
 */
void bcache_get_stats(bcache_stats_t *out);

/**
 * @brief Reset all cache statistics to zero.
 */
void bcache_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_BCACHE_H */
