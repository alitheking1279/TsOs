/**
 * @file bcache.c
 * @brief Buffer Cache — implementation.
 */

#include "bcache.h"
#include "block_dev.h"
#include "../drivers/serial.h"
#include "../kernel/spinlock.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Global State
 * ========================================================================= */

/** Static pool of buffers. */
static buf_t g_pool[BCACHE_NBUFS];

/** Hash table: array of chain heads. */
static buf_t *g_hash[BCACHE_HASH_SIZE];

/** LRU doubly-linked list.  lru_head is most-recent, lru_tail is oldest. */
static buf_t *g_lru_head;
static buf_t *g_lru_tail;

/** Global spinlock protecting all cache state. */
static spinlock_t g_lock = SPINLOCK_INIT;

/** Serial device for logging. */
static serial_dev_t *g_bc_serial = NULL;

/** Cache statistics. */
static bcache_stats_t g_stats;

/* =========================================================================
 * Logging
 * ========================================================================= */

static void bc_log(const char *msg) {
    if (g_bc_serial) serial_write_string(g_bc_serial, msg);
}

/* =========================================================================
 * Hash Function
 * ========================================================================= */

/**
 * @brief Hash (dev_id, block_num) into a bucket index.
 *
 * Uses a simple multiplicative hash to spread entries across buckets.
 */
static uint32_t bcache_hash(uint8_t dev_id, uint32_t block_num) {
    uint64_t key = ((uint64_t)dev_id << 32) | block_num;
    key = (key * 2654435761ULL) >> 48;  /* Knuth multiplicative hash */
    return (uint32_t)(key & (BCACHE_HASH_SIZE - 1));
}

/* =========================================================================
 * LRU List Operations
 * ========================================================================= */

/**
 * @brief Unlink a buffer from the LRU list.
 *
 * Does NOT modify the buffer's flags or ref_count.
 */
static void lru_remove(buf_t *b) {
    if (b->lru_prev) b->lru_prev->lru_next = b->lru_next;
    else             g_lru_head = b->lru_next;

    if (b->lru_next) b->lru_next->lru_prev = b->lru_prev;
    else             g_lru_tail = b->lru_prev;

    b->lru_prev = NULL;
    b->lru_next = NULL;
}

/**
 * @brief Push a buffer to the front of the LRU list (most recently used).
 */
static void lru_push_front(buf_t *b) {
    b->lru_prev = NULL;
    b->lru_next = g_lru_head;

    if (g_lru_head) g_lru_head->lru_prev = b;
    else            g_lru_tail = b;

    g_lru_head = b;
}

/**
 * @brief Move a buffer to the front of the LRU list.
 */
static void lru_touch(buf_t *b) {
    lru_remove(b);
    lru_push_front(b);
}

/* =========================================================================
 * Hash Table Operations
 * ========================================================================= */

/**
 * @brief Find a buffer in the hash table.
 *
 * @return Pointer to the buffer, or NULL if not found.
 */
static buf_t *hash_find(uint8_t dev_id, uint32_t block_num) {
    uint32_t bucket = bcache_hash(dev_id, block_num);
    buf_t *b = g_hash[bucket];

    while (b) {
        if (b->dev_id == dev_id && b->block_num == block_num)
            return b;
        b = b->hash_next;
    }
    return NULL;
}

/**
 * @brief Insert a buffer into the hash table.
 */
static void hash_insert(buf_t *b) {
    uint32_t bucket = bcache_hash(b->dev_id, b->block_num);
    b->hash_next = g_hash[bucket];
    g_hash[bucket] = b;
}

/**
 * @brief Remove a buffer from the hash table.
 */
static void hash_remove(buf_t *b) {
    uint32_t bucket = bcache_hash(b->dev_id, b->block_num);
    buf_t **pp = &g_hash[bucket];

    while (*pp) {
        if (*pp == b) {
            *pp = b->hash_next;
            b->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

/* =========================================================================
 * Initialization
 * ========================================================================= */

void bcache_init(void *serial_dev) {
    g_bc_serial = (serial_dev_t *)serial_dev;

    memset(g_pool, 0, sizeof(g_pool));
    memset(g_hash, 0, sizeof(g_hash));

    g_lru_head = NULL;
    g_lru_tail = NULL;

    /* Initialize all buffers as free and append to the LRU tail
     * so the first buffer in the pool is the first to be evicted. */
    for (int i = BCACHE_NBUFS - 1; i >= 0; i--) {
        g_pool[i].dev_id = 0;
        g_pool[i].block_num = 0;
        g_pool[i].flags = 0;
        g_pool[i].ref_count = 0;
        g_pool[i].lru_prev = NULL;
        g_pool[i].lru_next = NULL;
        g_pool[i].hash_next = NULL;
        lru_push_front(&g_pool[i]);
    }

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.total_buffers = BCACHE_NBUFS;

    bc_log("[BCACHE] Buffer cache initialized (");
    /* Print buffer count as decimal. */
    char numbuf[8];
    int n = BCACHE_NBUFS;
    int len = 0;
    if (n == 0) { numbuf[len++] = '0'; }
    else {
        char tmp[8];
        int tlen = 0;
        while (n > 0) { tmp[tlen++] = '0' + (n % 10); n /= 10; }
        for (int j = tlen - 1; j >= 0; j--) numbuf[len++] = tmp[j];
    }
    numbuf[len] = '\0';
    bc_log(numbuf);
    bc_log(" buffers).\r\n");
}

/* =========================================================================
 * Buffer Allocation (internal)
 * ========================================================================= */

/**
 * @brief Find a free buffer for eviction.
 *
 * Walks the LRU list from tail (oldest) to head, looking for a buffer
 * with ref_count == 0.  Returns the first such buffer found.
 */
static buf_t *find_evictable(void) {
    buf_t *b = g_lru_tail;
    while (b) {
        if (b->ref_count == 0) return b;
        b = b->lru_prev;
    }
    return NULL;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

buf_t *bcache_get(uint8_t dev_id, uint32_t block_num) {
    uint64_t rflags = spin_lock(&g_lock);

    /* 1. Look up in hash table. */
    buf_t *b = hash_find(dev_id, block_num);
    if (b) {
        g_stats.hash_hits++;
        b->ref_count++;
        lru_touch(b);
        spin_unlock(&g_lock, rflags);
        return b;
    }

    g_stats.hash_misses++;

    /* 2. Allocate a buffer: first try an unused one, else evict LRU. */
    b = find_evictable();
    if (!b) {
        spin_unlock(&g_lock, rflags);
        return NULL;  /* All buffers pinned — cannot allocate. */
    }

    /* 3. If the evicted buffer is dirty, write it back. */
    if ((b->flags & BCACHE_FLAG_VALID) && (b->flags & BCACHE_FLAG_DIRTY)) {
        /* Temporarily release lock for I/O (block_dev_write is thread-safe). */
        spin_unlock(&g_lock, rflags);

        block_status_t st = block_dev_write(b->dev_id, b->block_num, 1, b->data);
        if (st != BLOCK_OK) {
            bc_log("[BCACHE] WARN: flush-on-evict failed\r\n");
            /* Continue anyway — we discard the dirty data. */
        }
        g_stats.flushes++;

        rflags = spin_lock(&g_lock);
    }

    /* 4. Remove old hash entry if the buffer was valid. */
    if (b->flags & BCACHE_FLAG_VALID) {
        hash_remove(b);
        g_stats.evictions++;
    }

    /* 5. Set up the buffer for the new (dev_id, block_num). */
    b->dev_id = dev_id;
    b->block_num = block_num;
    b->flags = 0;
    b->ref_count = 1;

    hash_insert(b);
    lru_touch(b);

    /* 6. Read the sector from disk.  Release lock during I/O. */
    spin_unlock(&g_lock, rflags);

    block_status_t st = block_dev_read(dev_id, block_num, 1, b->data);
    if (st != BLOCK_OK) {
        /* Mark invalid and return NULL. */
        rflags = spin_lock(&g_lock);
        b->flags = 0;
        b->ref_count = 0;
        hash_remove(b);
        lru_touch(b);  /* Move to front so it's evicted soon. */
        spin_unlock(&g_lock, rflags);
        return NULL;
    }

    rflags = spin_lock(&g_lock);
    b->flags |= BCACHE_FLAG_VALID;
    g_stats.valid_buffers++;
    spin_unlock(&g_lock, rflags);

    return b;
}

void bcache_put(buf_t *buf) {
    if (!buf) return;

    uint64_t rflags = spin_lock(&g_lock);
    if (buf->ref_count > 0) {
        buf->ref_count--;
    }

    /* Write-through: when the last reference is released and the buffer
     * is dirty, write it back to the device immediately.  This guarantees
     * writes reach the disk even on an unclean shutdown (e.g. closing the
     * QEMU window), so files/directories created in the shell persist. */
    if (buf->ref_count == 0 &&
        (buf->flags & BCACHE_FLAG_VALID) &&
        (buf->flags & BCACHE_FLAG_DIRTY)) {
        uint8_t dev = buf->dev_id;
        uint32_t blk = buf->block_num;
        uint8_t copy[BCACHE_SECTOR_SIZE];
        memcpy(copy, buf->data, BCACHE_SECTOR_SIZE);
        spin_unlock(&g_lock, rflags);

        block_status_t st = block_dev_write(dev, blk, 1, copy);
        if (st == BLOCK_OK) {
            rflags = spin_lock(&g_lock);
            buf->flags &= ~BCACHE_FLAG_DIRTY;
            g_stats.flushes++;
            spin_unlock(&g_lock, rflags);
            return;
        }

        /* Leave the buffer dirty so a later flush/eviction retries it. */
        bc_log("[BCACHE] WARN: write-through failed, keeping dirty\r\n");
        return;
    }

    spin_unlock(&g_lock, rflags);
}

void bcache_dirty(buf_t *buf) {
    if (!buf) return;

    uint64_t rflags = spin_lock(&g_lock);
    buf->flags |= BCACHE_FLAG_DIRTY;
    g_stats.dirty_buffers++;
    spin_unlock(&g_lock, rflags);
}

bcache_status_t bcache_flush_dev(uint8_t dev_id) {
    uint64_t rflags = spin_lock(&g_lock);

    for (int i = 0; i < BCACHE_NBUFS; i++) {
        if (g_pool[i].dev_id == dev_id &&
            (g_pool[i].flags & BCACHE_FLAG_VALID) &&
            (g_pool[i].flags & BCACHE_FLAG_DIRTY)) {

            /* Grab what we need, release lock for I/O. */
            uint32_t blk = g_pool[i].block_num;
            uint8_t *data = g_pool[i].data;
            spin_unlock(&g_lock, rflags);

            block_status_t st = block_dev_write(dev_id, blk, 1, data);
            if (st != BLOCK_OK) return BCACHE_ERR_IO;

            rflags = spin_lock(&g_lock);
            g_pool[i].flags &= ~BCACHE_FLAG_DIRTY;
            g_stats.flushes++;
        }
    }

    g_stats.dirty_buffers = 0;
    spin_unlock(&g_lock, rflags);
    return BCACHE_OK;
}

bcache_status_t bcache_flush_all(void) {
    uint64_t rflags = spin_lock(&g_lock);

    for (int i = 0; i < BCACHE_NBUFS; i++) {
        if ((g_pool[i].flags & BCACHE_FLAG_VALID) &&
            (g_pool[i].flags & BCACHE_FLAG_DIRTY)) {

            uint8_t dev = g_pool[i].dev_id;
            uint32_t blk = g_pool[i].block_num;
            uint8_t *data = g_pool[i].data;
            spin_unlock(&g_lock, rflags);

            block_status_t st = block_dev_write(dev, blk, 1, data);
            if (st != BLOCK_OK) return BCACHE_ERR_IO;

            rflags = spin_lock(&g_lock);
            g_pool[i].flags &= ~BCACHE_FLAG_DIRTY;
            g_stats.flushes++;
        }
    }

    g_stats.dirty_buffers = 0;
    spin_unlock(&g_lock, rflags);
    return BCACHE_OK;
}

void bcache_invalidate_dev(uint8_t dev_id) {
    uint64_t rflags = spin_lock(&g_lock);

    for (int i = 0; i < BCACHE_NBUFS; i++) {
        if (g_pool[i].dev_id == dev_id && g_pool[i].ref_count == 0) {
            hash_remove(&g_pool[i]);
            g_pool[i].flags = 0;
            g_pool[i].block_num = 0;
            lru_touch(&g_pool[i]);
        }
    }

    spin_unlock(&g_lock, rflags);
}

void bcache_invalidate_all(void) {
    uint64_t rflags = spin_lock(&g_lock);

    memset(g_hash, 0, sizeof(g_hash));

    for (int i = 0; i < BCACHE_NBUFS; i++) {
        g_pool[i].flags = 0;
        g_pool[i].dev_id = 0;
        g_pool[i].block_num = 0;
        g_pool[i].hash_next = NULL;
        lru_touch(&g_pool[i]);
    }

    spin_unlock(&g_lock, rflags);
}

void bcache_get_stats(bcache_stats_t *out) {
    if (!out) return;

    uint64_t rflags = spin_lock(&g_lock);

    /* Recompute live counts. */
    uint32_t valid = 0, dirty = 0, free = 0;
    for (int i = 0; i < BCACHE_NBUFS; i++) {
        if (g_pool[i].flags & BCACHE_FLAG_VALID) valid++;
        if (g_pool[i].flags & BCACHE_FLAG_DIRTY) dirty++;
        if (g_pool[i].ref_count == 0 && !(g_pool[i].flags & BCACHE_FLAG_VALID)) free++;
    }

    g_stats.valid_buffers = valid;
    g_stats.dirty_buffers = dirty;
    g_stats.free_buffers = free;

    *out = g_stats;
    spin_unlock(&g_lock, rflags);
}

void bcache_reset_stats(void) {
    uint64_t rflags = spin_lock(&g_lock);
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.total_buffers = BCACHE_NBUFS;
    spin_unlock(&g_lock, rflags);
}
