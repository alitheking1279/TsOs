/**
 * @file kheap.c
 * @brief Kernel Heap — page-granularity buddy allocator (production quality).
 *
 * Implements a binary buddy allocator over the kernel heap virtual address
 * range ([HEAP_BASE .. HEAP_END) = 16 MiB = 4096 pages).
 *
 * All heap pages are demand-paged: the page fault handler in main.c
 * allocates physical frames on first touch.  This file manages ONLY
 * the virtual address space (buddy tree, free lists, metadata);
 * physical allocation is handled by the page fault path.
 *
 * =====================================================================
 * Buddy allocator design
 * =====================================================================
 *
 *  12 levels (0–11).  Level k covers a block of 2^k pages.
 *   Level 0:  1 page    (4 KiB)
 *   Level 1:  2 pages   (8 KiB)
 *   …
 *   Level 11: 2048 pages (8 MiB)
 *
 *  Free lists are singly-linked intrusive lists: the first 8 bytes of
 *  every free block store the pointer to the next free block at the
 *  same level.  Those bytes belong to a not-yet-demand-paged region,
 *  so writing the pointer itself triggers the page fault that maps the
 *  frame.  Because we want to avoid touching memory in the free list,
 *  the free list is maintained externally using a non-intrusive approach:
 *  we store pointers in kernel .bss (no page faults).
 *
 * =====================================================================
 * Metadata arrays  (in kernel .bss — no heap memory consumed)
 * =====================================================================
 *
 *  g_block_level[page]:
 *    - 0 … BUDDY_MAX_LEVEL : page is the FIRST page of a FREE block
 *                             at that level.
 *    - PAGE_ALLOCATED       : page is part of an ALLOCATED block.
 *    - PAGE_INTERIOR        : page is in the interior of a free block
 *                             (not the first page).
 *    Only the first page of each free/allocated block has a meaningful
 *    level tag; interior pages are PAGE_INTERIOR.
 *
 *  g_alloc_level[page]:
 *    Stored for EVERY page of every ALLOCATED block (level + 1 encoding).
 *    A value of 0 means the page is free or an interior page (not a block
 *    head that was directly allocated).  Values 1–12 correspond to buddy
 *    levels 0–11.  This allows kfree() to recover the allocation level
 *    from ANY page within a block and compute the block head correctly.
 *
 * =====================================================================
 * Correctness guarantees
 * =====================================================================
 *   - kmalloc(0) returns a valid 1-page allocation (≥ POSIX behaviour).
 *   - kfree(NULL) is a safe no-op.
 *   - Double-free is detected via g_block_level[page] != PAGE_ALLOCATED.
 *   - Out-of-range / misaligned pointers are rejected silently with a
 *     log message.
 *   - Allocation beyond heap capacity returns NULL (OOM).
 *   - Every split updates ALL page-level tags for the new right-half
 *     block (not just the first page).
 *   - Every free updates ALL page-level tags for the freed block before
 *     coalescing begins.
 *   - Coalescing only merges at the correct alignment boundary (the
 *     lower-addressed buddy must be aligned to 2^(level+1) pages).
 */

#include "kheap.h"
#include "page_table.h"
#include "pmm.h"
#include "vmm.h"
#include "spinlock.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define BUDDY_MAX_LEVEL    11u          /* 2^11 = 2048 pages = 8 MiB      */
#define BUDDY_NUM_LEVELS   (BUDDY_MAX_LEVEL + 1u)

/* Sentinel values for g_block_level[] */
#define PAGE_ALLOCATED  ((uint8_t)0xFF) /* page belongs to an allocation   */
#define PAGE_INTERIOR   ((uint8_t)0xFE) /* page is interior of a free block */

/* =========================================================================
 * Free-list node — NON-intrusive: stored in .bss, not in heap pages.
 *
 * We keep a simple singly-linked free list per level using small node
 * structs allocated from a statically-sized pool.  No heap memory is
 * consumed, so no page faults are triggered by free-list manipulation.
 * ========================================================================= */

/* Maximum number of simultaneously free blocks across all levels.
 * In the worst case every page is individually free: 4096 blocks at
 * level 0.  We size the pool to twice that for safety. */
#define FREE_NODE_POOL_SIZE  8192

typedef struct free_node {
    uint32_t        page_idx;   /* first page of this free block            */
    struct free_node *next;     /* next node on the same level's free list  */
} free_node_t;

static free_node_t   g_node_pool[FREE_NODE_POOL_SIZE];
static free_node_t  *g_pool_free;           /* singly-linked recycler       */
static free_node_t  *g_free_list[BUDDY_NUM_LEVELS]; /* per-level heads      */

/* =========================================================================
 * Per-page metadata
 * ========================================================================= */

/** Block level tag for each page.
 *  PAGE_ALLOCATED  — page is in an allocated block.
 *  PAGE_INTERIOR   — page is in the interior of a free block.
 *  0..11           — page is the FIRST page of a free block at that level. */
static uint8_t  g_block_level[HEAP_PAGE_COUNT];

/** Allocation level for EVERY page of each ALLOCATED block (level + 1).
 *  Encoded as level+1 so that 0 = free/interior (never allocated as a head).
 *  Valid range: 1..12 (levels 0..11). */
static uint8_t  g_alloc_level[HEAP_PAGE_COUNT];

/* =========================================================================
 * Statistics
 * ========================================================================= */

static uint64_t g_allocated_pages; /* pages currently held by allocations  */
static uint64_t g_total_allocs;    /* lifetime kmalloc() call count         */
static uint64_t g_active_allocs;   /* live (not yet freed) allocations      */

/** Snapshot taken at end of kheap_init(), before any kmalloc calls. */
static kheap_stats_t g_kheap_init_stats;

/* =========================================================================
 * Initialization guard and locking
 * ========================================================================= */

static int g_initialized;

/* Heap spinlock — protects buddy free lists and metadata.
 * All kmalloc/kfree operations acquire this lock.  Interrupts are
 * disabled by spinlock_acquire to prevent timer ISR deadlocks. */
static spinlock_t g_kheap_lock;

/* =========================================================================
 * Serial logging
 * ========================================================================= */

static serial_dev_t *g_kheap_serial;

static void kh_str(const char *s) {
    if (g_kheap_serial) serial_write_string(g_kheap_serial, s);
}

static void kh_hex64(uint64_t val) {
    static const char hex[] = "0123456789ABCDEF";
    if (!g_kheap_serial) return;
    serial_write_string(g_kheap_serial, "0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_write_char(g_kheap_serial, hex[(val >> i) & 0xF]);
}

static void kh_uint64(uint64_t val) {
    if (!g_kheap_serial) return;
    if (val == 0) { serial_write_char(g_kheap_serial, '0'); return; }
    char buf[21]; int i = 20; buf[i] = '\0';
    while (val > 0) { buf[--i] = (char)('0' + (val % 10)); val /= 10; }
    serial_write_string(g_kheap_serial, &buf[i]);
}

/* =========================================================================
 * Free-node pool management  (O(1) alloc / free)
 * ========================================================================= */

static void pool_init(void) {
    /* Link every node into the recycler list. */
    for (int i = 0; i < FREE_NODE_POOL_SIZE - 1; i++)
        g_node_pool[i].next = &g_node_pool[i + 1];
    g_node_pool[FREE_NODE_POOL_SIZE - 1].next = NULL;
    g_pool_free = &g_node_pool[0];
}

static free_node_t *pool_alloc(void) {
    if (!g_pool_free) return NULL;   /* pool exhausted — shouldn't happen */
    free_node_t *n = g_pool_free;
    g_pool_free = n->next;
    n->next = NULL;
    return n;
}

static void pool_free(free_node_t *n) {
    n->next     = g_pool_free;
    g_pool_free = n;
}

/* =========================================================================
 * Address ↔ page-index conversion
 * ========================================================================= */

static inline uint32_t vaddr_to_page(uint64_t vaddr) {
    return (uint32_t)((vaddr - HEAP_BASE) >> 12);  /* divide by PAGE_SIZE */
}

static inline uint64_t page_to_vaddr(uint32_t page) {
    return HEAP_BASE + ((uint64_t)page << 12);
}

/* =========================================================================
 * Free-list operations  (all O(n) at worst but list length is bounded)
 * ========================================================================= */

/**
 * Push a free block onto the front of the level's free list.
 * Returns false if the node pool is exhausted (a fatal condition).
 */
static int fl_push(uint32_t level, uint32_t page_idx) {
    free_node_t *n = pool_alloc();
    if (!n) {
        kh_str("[KHEAP] FATAL: free-node pool exhausted\r\n");
        return 0;
    }
    n->page_idx = page_idx;
    n->next     = g_free_list[level];
    g_free_list[level] = n;
    return 1;
}

/**
 * Remove the block with the given page_idx from the level's free list.
 * Returns 1 if found and removed, 0 if not found.
 */
static int fl_remove(uint32_t level, uint32_t page_idx) {
    free_node_t **pp = &g_free_list[level];
    while (*pp) {
        if ((*pp)->page_idx == page_idx) {
            free_node_t *dead = *pp;
            *pp = dead->next;
            pool_free(dead);
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;   /* not found */
}

/**
 * Pop and return the first block's page_idx from the level's free list.
 * Returns HEAP_PAGE_COUNT if the list is empty (invalid sentinel).
 */
static uint32_t fl_pop(uint32_t level) {
    free_node_t *n = g_free_list[level];
    if (!n) return HEAP_PAGE_COUNT;   /* sentinel: empty */
    g_free_list[level] = n->next;
    uint32_t pg = n->page_idx;
    pool_free(n);
    return pg;
}

/** True if the free list at level is non-empty. */
static inline int fl_has(uint32_t level) {
    return g_free_list[level] != NULL;
}

/* =========================================================================
 * Block-level tag helpers
 *
 * We tag every page in a block on every state transition to keep the
 * metadata consistent.  This is O(2^level) per operation but since
 * level ≤ 11 and 2^11 = 2048, it is at most 2048 byte writes.
 * ========================================================================= */

/** Mark all pages in block [start, start + 2^level) as free at `level`.
 *  Also clears g_alloc_level for all pages so that kfree() on an
 *  interior page of a free block is properly detected. */
static void mark_block_free(uint32_t start, uint32_t level) {
    uint32_t count = 1u << level;
    g_block_level[start] = (uint8_t)level;
    g_alloc_level[start] = 0;
    for (uint32_t i = 1; i < count; i++) {
        g_block_level[start + i] = PAGE_INTERIOR;
        g_alloc_level[start + i] = 0;
    }
}

/** Mark all pages in block [start, start + 2^level) as allocated.
 *  g_alloc_level is set to (level + 1) for ALL pages in the block,
 *  not just the head.  This allows kfree() to recover the level from
 *  ANY page within the block and compute the block head correctly. */
static void mark_block_allocated(uint32_t start, uint32_t level) {
    uint32_t count = 1u << level;
    uint8_t encoded = (uint8_t)(level + 1);
    for (uint32_t i = 0; i < count; i++) {
        g_block_level[start + i] = PAGE_ALLOCATED;
        g_alloc_level[start + i] = encoded;
    }
}

/* =========================================================================
 * Buddy address calculation
 *
 * For a block starting at page `p` at buddy level `k`:
 *   buddy_page = p XOR 2^k
 *
 * The XOR flips the k-th bit of the page index, which corresponds to
 * XOR-ing the (k+12)-th bit of the virtual address — exactly the standard
 * buddy address formula.
 *
 * For coalescing to be valid, the merged block must start at the lower
 * address and be aligned to 2^(k+1) pages.  We check:
 *   (min(p, buddy)) % 2^(k+1) == 0
 * ========================================================================= */

static inline uint32_t buddy_of(uint32_t page_idx, uint32_t level) {
    return page_idx ^ (1u << level);
}

/* =========================================================================
 * Minimum level needed for `pages` pages
 * ========================================================================= */

static uint32_t min_level_for_pages(uint64_t pages) {
    if (pages == 0) pages = 1;
    uint32_t level = 0;
    uint64_t block_size = 1;
    while (block_size < pages && level < BUDDY_MAX_LEVEL) {
        level++;
        block_size <<= 1;
    }
    return level;
}

/* =========================================================================
 * Core: split
 *
 * Split a block at page `start` from `from_level` down to `to_level`.
 * Each split:
 *   1. Compute right half = start + 2^(lvl-1).
 *   2. Tag entire right half as free at level (lvl-1).
 *   3. Push right half onto free list at level (lvl-1).
 *   4. Retag the left half (still at `start`) to the new level (lvl-1).
 *
 * On entry the block at `start` has already been removed from its
 * free list at `from_level`.  On return the block at `start` is at
 * `to_level` and NOT on any free list (ready for allocation).
 * ========================================================================= */
static void buddy_split(uint32_t start, uint32_t from_level, uint32_t to_level) {
    uint32_t cur_start = start;
    for (uint32_t lvl = from_level; lvl > to_level; lvl--) {
        uint32_t child_level = lvl - 1;
        uint32_t half        = 1u << child_level;    /* 2^(lvl-1) pages */
        uint32_t right       = cur_start + half;

        /* Tag right half as free at child_level. */
        mark_block_free(right, child_level);
        fl_push(child_level, right);

        /* Left half stays at cur_start; retag to child_level.
         * Only the first page tag needs updating (interior pages remain
         * PAGE_INTERIOR at the new smaller block; but since from_level
         * was larger, all pages in [cur_start, cur_start+2^lvl) already
         * had interior tags set by the previous iteration's mark_block_free
         * except for cur_start itself). */
        g_block_level[cur_start] = (uint8_t)child_level;
        /* Pages [cur_start+1, cur_start+half) are already PAGE_INTERIOR
         * from when the original block was tagged — no change needed. */
    }
}

/* =========================================================================
 * Core: coalesce
 *
 * After placing a block at `start` / `level` on the free list, attempt
 * to merge it with its buddy.  Repeat upward until level == MAX or
 * buddy is not free.
 *
 * On entry the block is ALREADY on g_free_list[level] and tagged.
 * On exit the block (possibly merged) is on the highest achievable free list.
 * ========================================================================= */
static void buddy_coalesce(uint32_t start, uint32_t level) {
    while (level < BUDDY_MAX_LEVEL) {
        uint32_t buddy = buddy_of(start, level);

        /* Buddy must be within the heap. */
        if (buddy >= HEAP_PAGE_COUNT) break;

        /* Buddy must be free at the same level. */
        if (g_block_level[buddy] != (uint8_t)level) break;

        /* Remove BOTH from the current level's free list. */
        fl_remove(level, start);
        fl_remove(level, buddy);

        /* Merged block starts at the lower address. */
        uint32_t merged = (buddy < start) ? buddy : start;

        /* Tag the merged block as free at (level+1). */
        level++;
        mark_block_free(merged, level);

        /* Push merged block onto free list at new level. */
        fl_push(level, merged);

        start = merged;
    }
}

/* =========================================================================
 * Demand PTE Setup
 *
 * The heap is demand-paged: no physical frames are allocated until the
 * first access.  However, the page table STRUCTURE (intermediate tables
 * at PDPT/PD/PT levels) must exist so the page fault handler can walk
 * to the leaf PTE and find PTE_DEMAND.
 *
 * This function walks the page table hierarchy for the entire heap
 * virtual address range [HEAP_BASE, HEAP_END) and creates DEMAND PTEs
 * at the leaf level.  Intermediate tables are created on-demand by
 * pt_walk_to_leaf().
 *
 * References:
 *   Intel SDM Vol.3A §4.5   — 4-level paging structure
 *   Intel SDM Vol.3A §4.6    — Translation process
 *   Intel SDM Vol.3A §6.15   — Page-Fault Exception (#PF)
 * ========================================================================= */

static void kheap_setup_demand_ptes(void) {
    address_space_t *kspace = vmm_get_kernel_address_space();
    if (!kspace) {
        kh_str("[KHEAP] ERROR: no kernel address space for demand PTE setup\r\n");
        return;
    }

    /*
     * Flags for intermediate page table entries: PRESENT | WRITABLE.
     * Intermediate tables (PML4E→PDPE, PDPE→PDE, PDE→PTE) are always
     * writable since the heap is a read-write region.
     */
    #define KHEAP_PT_DEFAULT_FLAGS (PTE_PRESENT | PTE_WRITABLE)

    uint32_t demand_count = 0;

    for (uint64_t va = HEAP_BASE; va < HEAP_END; va += PAGE_SIZE) {
        uint64_t *pte = pt_walk_to_leaf(kspace->pml4, va,
                                         true, KHEAP_PT_DEFAULT_FLAGS);
        if (!pte) {
            kh_str("[KHEAP] ERROR: failed to create PTE for VA ");
            kh_hex64(va);
            kh_str("\r\n");
            return;
        }

        /*
         * Set the leaf PTE to DEMAND | WRITABLE but NOT PRESENT.
         * The page fault handler will resolve this on first touch by
         * allocating a physical frame and setting PTE_PRESENT.
         *
         * Bits layout of a DEMAND PTE:
         *   Bit 0  (PTE_PRESENT)  = 0  (not yet mapped)
         *   Bit 1  (PTE_WRITABLE) = 1  (heap is read-write)
         *   Bit 10 (PTE_DEMAND)   = 1  (demand-paged marker)
         *   Bits 12-51             = 0  (no physical frame yet)
         */
        *pte = PTE_DEMAND | PTE_WRITABLE;
        demand_count++;
    }

    #undef KHEAP_PT_DEFAULT_FLAGS

    kh_str("[KHEAP] Demand PTEs  : ");
    kh_uint64(demand_count);
    kh_str(" entries created\r\n");
}

/* =========================================================================
 * kheap_init
 * ========================================================================= */

void kheap_init(void *serial_dev) {
    g_kheap_serial = (serial_dev_t *)serial_dev;
    spinlock_init(&g_kheap_lock);

    kh_str("\r\n[KHEAP] ============================================\r\n");
    kh_str("[KHEAP] Kernel Heap initializing...\r\n");

    /* ---- Step 1: Set up DEMAND PTEs for the heap range ----
     *
     * The page table structure must exist before any heap access
     * triggers a page fault.  Without this, the PF handler cannot
     * walk to the leaf PTE and the system triple-faults.
     */
    kheap_setup_demand_ptes();

    /* ---- Initialize pool ---- */
    pool_init();

    /* ---- Clear all free lists ---- */
    for (uint32_t i = 0; i < BUDDY_NUM_LEVELS; i++)
        g_free_list[i] = NULL;

    /* ---- Mark all pages as allocated initially (none free yet) ---- */
    for (uint32_t i = 0; i < HEAP_PAGE_COUNT; i++) {
        g_block_level[i] = PAGE_ALLOCATED;
        g_alloc_level[i] = 0;
    }

    /* ---- Release the entire 16 MiB heap in two level-11 blocks ----
     *
     * HEAP_PAGE_COUNT = 4096 pages.
     * 2^11 = 2048 pages = one half.
     * We add two halves at level 11.
     */
    uint32_t half  = 1u << BUDDY_MAX_LEVEL;   /* 2048 */
    uint32_t start0 = 0;
    uint32_t start1 = half;                   /* 2048 */

    mark_block_free(start0, BUDDY_MAX_LEVEL);
    fl_push(BUDDY_MAX_LEVEL, start0);

    mark_block_free(start1, BUDDY_MAX_LEVEL);
    fl_push(BUDDY_MAX_LEVEL, start1);

    /* ---- Stats reset ---- */
    g_allocated_pages = 0;
    g_total_allocs    = 0;
    g_active_allocs   = 0;

    g_initialized = 1;

    /* ---- Capture init-time stats snapshot for Group A tests ---- */
    g_kheap_init_stats.total_pages     = HEAP_PAGE_COUNT;
    g_kheap_init_stats.allocated_pages = 0;
    g_kheap_init_stats.free_pages      = HEAP_PAGE_COUNT;
    g_kheap_init_stats.total_allocs    = 0;
    g_kheap_init_stats.active_allocs   = 0;

    kh_str("[KHEAP] Base      : "); kh_hex64(HEAP_BASE);       kh_str("\r\n");
    kh_str("[KHEAP] End       : "); kh_hex64(HEAP_END);         kh_str("\r\n");
    kh_str("[KHEAP] Pages     : "); kh_uint64(HEAP_PAGE_COUNT); kh_str("\r\n");
    kh_str("[KHEAP] Levels    : 0.."); kh_uint64(BUDDY_MAX_LEVEL); kh_str("\r\n");
    kh_str("[KHEAP] Init complete.\r\n");
    kh_str("[KHEAP] ============================================\r\n");
}

/* =========================================================================
 * kmalloc
 * ========================================================================= */

void *kmalloc(uint64_t size) {
    if (!g_initialized) return NULL;

    /* Round size up to pages; ensure at least 1 page. */
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;

    /* Find the buddy level that covers `pages` pages. */
    uint32_t need_level = min_level_for_pages(pages);

    uint64_t kh_rflags = spin_lock(&g_kheap_lock);

    /* Walk upward to find a level with a free block. */
    uint32_t alloc_level = need_level;
    while (alloc_level <= BUDDY_MAX_LEVEL && !fl_has(alloc_level))
        alloc_level++;

    if (alloc_level > BUDDY_MAX_LEVEL) {
        spin_unlock(&g_kheap_lock, kh_rflags);
        kh_str("[KHEAP] kmalloc("); kh_uint64(size); kh_str(") = NULL (OOM)\r\n");
        return NULL;
    }

    /* Pop the free block. */
    uint32_t pg = fl_pop(alloc_level);
    if (pg >= HEAP_PAGE_COUNT) {
        spin_unlock(&g_kheap_lock, kh_rflags);
        kh_str("[KHEAP] kmalloc: internal error — fl_pop returned invalid page\r\n");
        return NULL;
    }

    /* Split down to need_level if we grabbed a larger block. */
    if (alloc_level > need_level) {
        buddy_split(pg, alloc_level, need_level);
    }

    /* Mark the entire 2^need_level block as allocated. */
    mark_block_allocated(pg, need_level);

    /* Update statistics inside the lock — keeps them consistent with
     * the allocator state they reflect (single-core safe; SMP-safe with lock). */
    uint32_t block_pages = 1u << need_level;
    g_allocated_pages += block_pages;
    g_total_allocs++;
    g_active_allocs++;

    spin_unlock(&g_kheap_lock, kh_rflags);

    uint64_t vaddr = page_to_vaddr(pg);

    kh_str("[KHEAP] kmalloc(");  kh_uint64(size);
    kh_str(") = ");              kh_hex64(vaddr);
    kh_str(" [");                kh_uint64(block_pages);
    kh_str(" pages, level ");    kh_uint64(need_level);
    kh_str("]\r\n");

    return (void *)vaddr;
}

/* =========================================================================
 * kfree
 * ========================================================================= */

void kfree(void *ptr) {
    if (!g_initialized) return;
    if (!ptr) return;

    uint64_t vaddr = (uint64_t)ptr;

    /* Range check. */
    if (vaddr < HEAP_BASE || vaddr >= HEAP_END) {
        kh_str("[KHEAP] kfree("); kh_hex64(vaddr);
        kh_str(") — address out of heap range, ignoring\r\n");
        return;
    }

    /* Alignment check. */
    if (!is_page_aligned(vaddr)) {
        kh_str("[KHEAP] kfree("); kh_hex64(vaddr);
        kh_str(") — address not page-aligned, ignoring\r\n");
        return;
    }

    uint32_t pg = vaddr_to_page(vaddr);

    uint64_t kh_rflags = spin_lock(&g_kheap_lock);

    /* Validate the page is in an allocated block using the level+1 encoding.
     * g_alloc_level[pg] == 0 means the page is free or an interior page.
     * g_alloc_level[pg] >= 1 means it's part of an allocated block at
     * level (g_alloc_level[pg] - 1). */
    if (g_block_level[pg] != PAGE_ALLOCATED || g_alloc_level[pg] == 0) {
        spin_unlock(&g_kheap_lock, kh_rflags);
        kh_str("[KHEAP] kfree("); kh_hex64(vaddr);
        kh_str(") — page is not in an allocated block (double-free?), ignoring\r\n");
        return;
    }

    /* Recover the allocation level and compute the block head.
     * The head is the block-aligned start address for this level. */
    uint32_t level = (uint32_t)g_alloc_level[pg] - 1;
    uint32_t head  = pg & ~((1u << level) - 1);

    uint32_t block_pages = 1u << level;

    /* Tag block as free (must be done BEFORE coalescing checks).
     * Always free from the block head, not from an interior page. */
    mark_block_free(head, level);

    /* Add to free list. */
    fl_push(level, head);

    /* Coalesce with buddy upward. */
    buddy_coalesce(head, level);

    /* Update statistics inside the lock — symmetric with kmalloc path. */
    g_allocated_pages -= block_pages;
    g_active_allocs--;

    spin_unlock(&g_kheap_lock, kh_rflags);

    kh_str("[KHEAP] kfree(");   kh_hex64(vaddr);
    kh_str(") [");              kh_uint64(block_pages);
    kh_str(" pages, level ");   kh_uint64(level);
    if (head != pg) {
        kh_str(", head ");      kh_hex64(page_to_vaddr(head));
    }
    kh_str("]\r\n");
}


/* =========================================================================
 * kheap_get_init_stats
 * ========================================================================= */

void kheap_get_init_stats(kheap_stats_t *out) {
    if (!out) return;
    *out = g_kheap_init_stats;
}

/* =========================================================================
 * kheap_get_stats
 * ========================================================================= */

void kheap_get_stats(kheap_stats_t *out) {
    if (!out) return;
    out->total_pages     = HEAP_PAGE_COUNT;
    out->allocated_pages = g_allocated_pages;
    out->free_pages      = HEAP_PAGE_COUNT - g_allocated_pages;
    out->total_allocs    = g_total_allocs;
    out->active_allocs   = g_active_allocs;
}

/* =========================================================================
 * kheap_verify_integrity  (debug / test helper)
 *
 * Walk every free list and verify:
 *   (a) Every node's page_idx is within [0, HEAP_PAGE_COUNT).
 *   (b) The page's g_block_level tag matches the list level.
 *   (c) No page appears on more than one free list.
 *   (d) The free page count computed from the free lists equals
 *       (HEAP_PAGE_COUNT - g_allocated_pages).
 *
 * Returns 1 if all invariants hold, 0 otherwise.
 * Logs all violations to the serial port.
 * ========================================================================= */
int kheap_verify_integrity(void) {
    if (!g_initialized) {
        kh_str("[KHEAP VERIFY] Not initialized.\r\n");
        return 0;
    }

    /* visited[page] = 1 if we have seen this page on a free list. */
    /* Use a stack-local array — 4096 bytes is fine on kernel stack. */
    static uint8_t visited[HEAP_PAGE_COUNT];
    for (uint32_t i = 0; i < HEAP_PAGE_COUNT; i++) visited[i] = 0;

    int ok = 1;
    uint64_t free_pages_from_lists = 0;

    for (uint32_t lvl = 0; lvl <= BUDDY_MAX_LEVEL; lvl++) {
        uint32_t block_size = 1u << lvl;
        for (free_node_t *n = g_free_list[lvl]; n; n = n->next) {
            uint32_t pg = n->page_idx;

            /* (a) Range check. */
            if (pg >= HEAP_PAGE_COUNT) {
                kh_str("[KHEAP VERIFY] FAIL: level "); kh_uint64(lvl);
                kh_str(" node has out-of-range page "); kh_uint64(pg); kh_str("\r\n");
                ok = 0; continue;
            }

            /* (b) Tag check. */
            if (g_block_level[pg] != (uint8_t)lvl) {
                kh_str("[KHEAP VERIFY] FAIL: page "); kh_uint64(pg);
                kh_str(" tag="); kh_uint64(g_block_level[pg]);
                kh_str(" but is on level-"); kh_uint64(lvl); kh_str(" list\r\n");
                ok = 0;
            }

            /* (c) Duplicate check. */
            if (visited[pg]) {
                kh_str("[KHEAP VERIFY] FAIL: page "); kh_uint64(pg);
                kh_str(" appears on multiple free lists\r\n");
                ok = 0;
            }
            visited[pg] = 1;

            /* Count free pages contributed by this block. */
            /* Make sure interior pages are correctly tagged. */
            for (uint32_t i = 1; i < block_size && (pg + i) < HEAP_PAGE_COUNT; i++) {
                if (g_block_level[pg + i] != PAGE_INTERIOR) {
                    kh_str("[KHEAP VERIFY] FAIL: interior page ");
                    kh_uint64(pg + i);
                    kh_str(" has unexpected tag ");
                    kh_uint64(g_block_level[pg + i]); kh_str("\r\n");
                    ok = 0;
                }
            }

            free_pages_from_lists += block_size;
        }
    }

    /* (d) Consistency check. */
    uint64_t expected_free = HEAP_PAGE_COUNT - g_allocated_pages;
    if (free_pages_from_lists != expected_free) {
        kh_str("[KHEAP VERIFY] FAIL: free_pages_from_lists=");
        kh_uint64(free_pages_from_lists);
        kh_str(" expected="); kh_uint64(expected_free); kh_str("\r\n");
        ok = 0;
    }

    if (ok) {
        kh_str("[KHEAP VERIFY] All invariants OK. free_pages=");
        kh_uint64(free_pages_from_lists);
        kh_str(" alloc_pages="); kh_uint64(g_allocated_pages); kh_str("\r\n");
    }

    return ok;
}
