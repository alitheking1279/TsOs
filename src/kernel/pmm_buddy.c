#include "pmm_buddy.h"
#include "pmm.h"
#include "spinlock.h"
#include "../drivers/serial.h"
#include <stdint.h>

/* PMM buddy layer for efficient contiguous frame allocation.
 *
 * Maintains per-order free lists for blocks of 2^k pages.
 * On alloc: find the smallest order ≥ ceil(log2(count)) with a free block,
 * split down as needed. On free: coalesce with buddy if both free.
 *
 * The bitmap PMM is the backing store; buddy just manages the free lists
 * for contiguous allocations. Single-page allocations bypass this layer. */

typedef struct buddy_node {
    uint64_t addr;
    struct buddy_node *next;
} buddy_node_t;

/* Maximum number of simultaneously free blocks.
 * Worst case: all memory individually free = total_frames blocks.
 * We size the pool to twice the total frames for safety, with a minimum
 * of 8192 to handle fragmentation. */
#define BUDDY_NODE_POOL_MAX 16384

static buddy_node_t g_node_pool[BUDDY_NODE_POOL_MAX];
static buddy_node_t *g_pool_free;
static buddy_node_t *g_free_lists[PMM_BUDDY_MAX_ORDER];

static uint64_t g_total_frames;
static spinlock_t g_buddy_lock;
static serial_dev_t *g_buddy_serial;
static int g_buddy_initialized;

static void buddy_log(const char *s)
{
    if (g_buddy_serial) serial_write_string(g_buddy_serial, s);
}

static void buddy_log_hex(uint64_t v)
{
    if (!g_buddy_serial) return;
    static const char hex[] = "0123456789ABCDEF";
    serial_write_string(g_buddy_serial, "0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_write_char(g_buddy_serial, hex[(v >> i) & 0xF]);
}

static void buddy_log_u64(uint64_t v)
{
    if (!g_buddy_serial) return;
    if (v == 0) { serial_write_char(g_buddy_serial, '0'); return; }
    char buf[21]; int i = 20; buf[i] = '\0';
    while (v > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    serial_write_string(g_buddy_serial, &buf[i]);
}

static void pool_init(void)
{
    for (int i = 0; i < BUDDY_NODE_POOL_MAX - 1; i++)
        g_node_pool[i].next = &g_node_pool[i + 1];
    g_node_pool[BUDDY_NODE_POOL_MAX - 1].next = NULL;
    g_pool_free = &g_node_pool[0];
}

static buddy_node_t *pool_alloc(void)
{
    if (!g_pool_free) return NULL;
    buddy_node_t *n = g_pool_free;
    g_pool_free = n->next;
    n->next = NULL;
    return n;
}

static void pool_free(buddy_node_t *n)
{
    n->next = g_pool_free;
    g_pool_free = n;
}

static inline uint32_t order_for_pages(uint32_t pages)
{
    if (pages <= 1) return 0;
    uint32_t order = 0;
    uint32_t block = 1;
    while (block < pages) {
        order++;
        block <<= 1;
    }
    return order;
}

static void push(uint32_t order, uint64_t addr)
{
    buddy_node_t *n = pool_alloc();
    if (!n) return;
    n->addr = addr;
    n->next = g_free_lists[order];
    g_free_lists[order] = n;
}

static int remove(uint32_t order, uint64_t addr)
{
    buddy_node_t **pp = &g_free_lists[order];
    while (*pp) {
        if ((*pp)->addr == addr) {
            buddy_node_t *dead = *pp;
            *pp = dead->next;
            pool_free(dead);
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

static uint64_t pop(uint32_t order)
{
    buddy_node_t *n = g_free_lists[order];
    if (!n) return 0;
    g_free_lists[order] = n->next;
    uint64_t addr = n->addr;
    pool_free(n);
    return addr;
}

static uint64_t buddy_addr(uint64_t addr, uint32_t order)
{
    uint64_t block_size = (uint64_t)PAGE_SIZE << order;
    return addr ^ block_size;
}

void pmm_buddy_init(void *serial_dev)
{
    g_buddy_serial = (serial_dev_t *)serial_dev;
    spinlock_init(&g_buddy_lock);
    pool_init();

    for (uint32_t i = 0; i < PMM_BUDDY_MAX_ORDER; i++)
        g_free_lists[i] = NULL;

    pmm_stats_t stats;
    pmm_get_stats(&stats);
    g_total_frames = stats.total_frames;

    /* Seed the buddy free lists from the bitmap PMM.
     * Walk the bitmap and find runs of free frames. */
    uint64_t run_start = 0;
    uint64_t run_len = 0;

    for (uint64_t f = 0; f < g_total_frames; f++) {
        if (pmm_is_free(f << PAGE_SHIFT) == 1) {
            if (run_len == 0) run_start = f;
            run_len++;
        } else {
            if (run_len > 0) {
                /* Break this run into buddy-aligned blocks and free them. */
                uint64_t addr = run_start << PAGE_SHIFT;
                uint64_t remaining = run_len;

                while (remaining > 0) {
                    /* Find the largest buddy-aligned block that fits. */
                    uint32_t order = order_for_pages((uint32_t)remaining);
                    uint64_t block_pages = (uint64_t)1 << order;

                    /* Align check: block must start at a block_pages boundary. */
                    uint64_t page_idx = addr >> PAGE_SHIFT;
                    if (page_idx & (block_pages - 1)) {
                        /* Not aligned — try smaller order. */
                        order--;
                        if (order > PMM_BUDDY_MAX_ORDER) order = PMM_BUDDY_MAX_ORDER - 1;
                        block_pages = (uint64_t)1 << order;
                        if (page_idx & (block_pages - 1)) {
                            /* Still not aligned — decrement until aligned. */
                            while (order > 0 && (page_idx & ((1ULL << order) - 1)))
                                order--;
                            block_pages = (uint64_t)1 << order;
                        }
                    }

                    if (block_pages > remaining) {
                        while (block_pages > remaining && order > 0) {
                            order--;
                            block_pages = (uint64_t)1 << order;
                        }
                    }

                    if (block_pages <= remaining && block_pages <= PMM_BUDDY_MAX_PAGES) {
                        push(order, addr);
                        addr += block_pages * PAGE_SIZE;
                        remaining -= block_pages;
                    } else {
                        break;
                    }
                }
                run_len = 0;
            }
        }
    }

    /* Handle trailing run. */
    if (run_len > 0) {
        uint64_t addr = run_start << PAGE_SHIFT;
        uint64_t remaining = run_len;

        while (remaining > 0) {
            uint32_t order = order_for_pages((uint32_t)remaining);
            uint64_t block_pages = (uint64_t)1 << order;

            uint64_t page_idx = addr >> PAGE_SHIFT;
            while (order > 0 && (page_idx & ((1ULL << order) - 1)))
                order--;
            block_pages = (uint64_t)1 << order;

            while (block_pages > remaining && order > 0) {
                order--;
                block_pages = (uint64_t)1 << order;
            }

            if (block_pages <= remaining && block_pages <= PMM_BUDDY_MAX_PAGES) {
                push(order, addr);
                addr += block_pages * PAGE_SIZE;
                remaining -= block_pages;
            } else {
                break;
            }
        }
    }

    g_buddy_initialized = 1;

    buddy_log("[BUDDY] Init complete.\r\n");
}

uint64_t pmm_buddy_alloc_pages(uint32_t count)
{
    if (!g_buddy_initialized || count == 0) return 0;
    if (count == 1) return pmm_alloc_frame();
    if (count > PMM_BUDDY_MAX_PAGES) return 0;

    uint64_t buddy_rflags = spin_lock(&g_buddy_lock);

    uint32_t order = order_for_pages(count);
    uint32_t alloc_order = order;

    /* Find the smallest order with a free block. */
    while (alloc_order < PMM_BUDDY_MAX_ORDER) {
        uint64_t addr = pop(alloc_order);
        if (addr != 0) {
            /* Split down to the requested order. */
            for (uint32_t o = alloc_order; o > order; o--) {
                uint64_t buddy = buddy_addr(addr, o - 1);
                push(o - 1, buddy);
            }
            spin_unlock(&g_buddy_lock, buddy_rflags);

            /* Sync with bitmap PMM: mark all frames as USED. */
            uint64_t block_pages = (uint64_t)1 << order;
            for (uint64_t i = 0; i < block_pages; i++) {
                pmm_mark_used(addr + i * PAGE_SIZE);
            }

            buddy_log("[BUDDY] alloc ");
            buddy_log_u64(count);
            buddy_log(" pages -> ");
            buddy_log_hex(addr);
            buddy_log("\r\n");
            return addr;
        }
        alloc_order++;
    }

    /* Fallback: allocate individual frames from bitmap PMM. */
    spin_unlock(&g_buddy_lock, buddy_rflags);

    uint64_t fallback_addr = pmm_alloc_frames(count);
    if (fallback_addr != 0) {
        buddy_log("[BUDDY] alloc (fallback) ");
        buddy_log_u64(count);
        buddy_log(" pages -> ");
        buddy_log_hex(fallback_addr);
        buddy_log("\r\n");
        return fallback_addr;
    }

    return 0;
}

void pmm_buddy_free_pages(uint64_t addr, uint32_t count)
{
    if (!g_buddy_initialized || count == 0 || addr == 0) return;
    if (count == 1) {
        pmm_free_frame(addr);
        return;
    }

    uint64_t buddy_rflags = spin_lock(&g_buddy_lock);

    uint32_t order = order_for_pages(count);

    /* Coalesce loop: try to merge with buddy at each level. */
    uint64_t cur_addr = addr;
    uint32_t cur_order = order;

    while (cur_order < PMM_BUDDY_MAX_ORDER) {
        uint64_t b = buddy_addr(cur_addr, cur_order);

        /* Check if buddy is free at this order. */
        int found = 0;
        buddy_node_t **pp = &g_free_lists[cur_order];
        while (*pp) {
            if ((*pp)->addr == b) {
                buddy_node_t *dead = *pp;
                *pp = dead->next;
                pool_free(dead);
                found = 1;
                break;
            }
            pp = &(*pp)->next;
        }

        if (!found) break;

        /* Merge: take the lower address and go up one order. */
        cur_addr = (cur_addr < b) ? cur_addr : b;
        cur_order++;
    }

    push(cur_order, cur_addr);
    spin_unlock(&g_buddy_lock, buddy_rflags);

    /* Sync with bitmap PMM: mark all frames as FREE.
     * Use the final coalesced order to compute the correct block size,
     * not the original count. After coalescing, the block may span
     * a larger range (e.g. count=2 but coalesced to order=3 = 8 pages).
     * Marking only `count` frames would leave the extra pages still
     * marked USED in the bitmap, creating a buddy/bitmap inconsistency. */
    uint64_t final_count = (uint64_t)1 << cur_order;
    for (uint64_t i = 0; i < final_count; i++) {
        pmm_mark_free(cur_addr + i * PAGE_SIZE);
    }

    buddy_log("[BUDDY] free ");
    buddy_log_u64(count);
    buddy_log(" pages @ ");
    buddy_log_hex(addr);
    buddy_log("\r\n");
}

int pmm_buddy_verify(void)
{
    if (!g_buddy_initialized) return 0;

    uint64_t buddy_rflags = spin_lock(&g_buddy_lock);

    /* Count total free pages across all orders. */
    uint64_t total_free = 0;
    for (uint32_t o = 0; o < PMM_BUDDY_MAX_ORDER; o++) {
        uint64_t block_pages = (uint64_t)1 << o;
        buddy_node_t *n = g_free_lists[o];
        while (n) {
            total_free += block_pages;
            n = n->next;
        }
    }

    spin_unlock(&g_buddy_lock, buddy_rflags);

    buddy_log("[BUDDY VERIFY] total_free=");
    buddy_log_u64(total_free);
    buddy_log(" pages\r\n");
    return 1;
}
