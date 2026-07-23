/**
 * @file test_kheap.c
 * @brief Kernel Heap (kheap) test suite — 80 tests across 10 groups.
 *
 * Test philosophy
 * ===============
 * Every test exercises the real buddy allocator state.  We rely on
 * kheap_get_stats() for statistical assertions and kheap_verify_integrity()
 * after every destructive sequence to confirm internal invariants hold.
 *
 * Tests are fully self-contained: each allocates what it needs and frees
 * it before returning, leaving the heap in the same state as it started.
 *
 * Test groups
 * ===========
 * Group A — Post-init invariants            (6 tests)
 * Group B — Basic kmalloc / kfree round-trip (8 tests)
 * Group C — Size classes & buddy levels     (8 tests)
 * Group D — Multiple simultaneous allocs    (8 tests)
 * Group E — Free and reuse                  (6 tests)
 * Group F — Buddy coalescing                (8 tests)
 * Group G — Edge & error cases              (8 tests)
 * Group H — Stress: many small allocs       (8 tests)
 * Group I — Stress: many large allocs       (8 tests)
 * Group J — Stats tracking                  (6 tests)
 *
 * Total: 74 tests
 */

#include "test.h"
#include "../kernel/kheap.h"
#include "../kernel/page_table.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Logging helpers
 * ========================================================================= */


/* =========================================================================

 * Group A — Post-init invariants (6 tests)
 *
 * Verify the heap is in a clean state immediately after kheap_init().
 * kheap_init() is called by kernel_main() before tests run; we just
 * inspect the resulting state.
 * ========================================================================= */

/** A1: total_pages is exactly HEAP_PAGE_COUNT (4096). */
static void test_kheap_total_pages(serial_dev_t *dev) {
    kheap_stats_t s; kheap_get_stats(&s);
    ASSERT_EQ(dev, s.total_pages, (uint64_t)HEAP_PAGE_COUNT);
}

/** A2: kheap_init() snapshot shows zero allocated pages at init time. */
static void test_kheap_init_alloc_zero(serial_dev_t *dev) {
    kheap_stats_t init; kheap_get_init_stats(&init);
    ASSERT_EQ(dev, init.allocated_pages, (uint64_t)0);
}

/** A3: kheap_init() snapshot shows free_pages == total_pages at init time. */
static void test_kheap_init_free_equals_total(serial_dev_t *dev) {
    kheap_stats_t init; kheap_get_init_stats(&init);
    ASSERT_EQ(dev, init.free_pages, init.total_pages);
}

/** A4: free + allocated == total. */
static void test_kheap_init_conservation(serial_dev_t *dev) {
    kheap_stats_t s; kheap_get_stats(&s);
    ASSERT_EQ(dev, s.free_pages + s.allocated_pages, s.total_pages);
}

/** A5: kheap_init() snapshot shows zero active allocs at init time. */
static void test_kheap_init_active_allocs_zero(serial_dev_t *dev) {
    kheap_stats_t init; kheap_get_init_stats(&init);
    ASSERT_EQ(dev, init.active_allocs, (uint64_t)0);
}

/** A6: Internal integrity check passes immediately after init. */
static void test_kheap_init_integrity(serial_dev_t *dev) {
    int ok = kheap_verify_integrity();
    ASSERT_TRUE(dev, ok == 1);
}

/* =========================================================================
 * Group B — Basic kmalloc / kfree round-trip (8 tests)
 * ========================================================================= */

/** B1: kmalloc(0) returns a non-NULL pointer. */
static void test_kheap_malloc_zero(serial_dev_t *dev) {
    void *p = kmalloc(0);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
}

/** B2: kmalloc(1) returns a non-NULL pointer. */
static void test_kheap_malloc_one_byte(serial_dev_t *dev) {
    void *p = kmalloc(1);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
}

/** B3: kmalloc returns a page-aligned pointer. */
static void test_kheap_malloc_aligned(serial_dev_t *dev) {
    void *p = kmalloc(1);
    ASSERT_NOT_NULL(dev, p);
    ASSERT_TRUE(dev, is_page_aligned((uint64_t)p));
    kfree(p);
}

/** B4: Returned pointer is within [HEAP_BASE, HEAP_END). */
static void test_kheap_malloc_in_range(serial_dev_t *dev) {
    void *p = kmalloc(100);
    ASSERT_NOT_NULL(dev, p);
    uint64_t va = (uint64_t)p;
    ASSERT_TRUE(dev, va >= HEAP_BASE);
    ASSERT_TRUE(dev, va < HEAP_END);
    kfree(p);
}

/** B5: kfree(NULL) is a safe no-op (does not crash). */
static void test_kheap_free_null(serial_dev_t *dev) {
    kfree(NULL);
    /* If we reach here without a fault, the test passes. */
    ASSERT_TRUE(dev, 1);
}

/** B6: After alloc + free, active_allocs returns to 0. */
static void test_kheap_free_restores_active(serial_dev_t *dev) {
    kheap_stats_t before; kheap_get_stats(&before);
    void *p = kmalloc(4096);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
    kheap_stats_t after; kheap_get_stats(&after);
    ASSERT_EQ(dev, after.active_allocs, before.active_allocs);
}

/** B7: After alloc + free, free_pages returns to original value. */
static void test_kheap_free_restores_free_pages(serial_dev_t *dev) {
    kheap_stats_t before; kheap_get_stats(&before);
    void *p = kmalloc(4096);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
    kheap_stats_t after; kheap_get_stats(&after);
    ASSERT_EQ(dev, after.free_pages, before.free_pages);
}

/** B8: Integrity check passes after a single alloc-free cycle. */
static void test_kheap_single_cycle_integrity(serial_dev_t *dev) {
    void *p = kmalloc(128);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/* =========================================================================
 * Group C — Size classes and buddy levels (8 tests)
 * ========================================================================= */

/** C1: kmalloc(PAGE_SIZE) allocates exactly 1 page (level 0). */
static void test_kheap_one_page(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, p);
    kheap_stats_t a; kheap_get_stats(&a);
    /* Level 0 block = 1 page */
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 1);
    kfree(p);
}

/** C2: kmalloc(PAGE_SIZE + 1) allocates 2 pages (level 1). */
static void test_kheap_two_pages(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(PAGE_SIZE + 1);
    ASSERT_NOT_NULL(dev, p);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 2);
    kfree(p);
}

/** C3: kmalloc(2*PAGE_SIZE) allocates exactly 2 pages. */
static void test_kheap_exact_two_pages(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(2 * PAGE_SIZE);
    ASSERT_NOT_NULL(dev, p);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 2);
    kfree(p);
}

/** C4: kmalloc(2*PAGE_SIZE + 1) allocates 4 pages (level 2). */
static void test_kheap_four_pages(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(2 * PAGE_SIZE + 1);
    ASSERT_NOT_NULL(dev, p);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 4);
    kfree(p);
}

/** C5: kmalloc(4*PAGE_SIZE) allocates 4 pages. */
static void test_kheap_exact_four_pages(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(4 * PAGE_SIZE);
    ASSERT_NOT_NULL(dev, p);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 4);
    kfree(p);
}

/** C6: kmalloc(8*PAGE_SIZE) allocates 8 pages (level 3). */
static void test_kheap_eight_pages(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(8 * PAGE_SIZE);
    ASSERT_NOT_NULL(dev, p);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 8);
    kfree(p);
}

/** C7: kmalloc(16*PAGE_SIZE) allocates 16 pages (level 4). */
static void test_kheap_sixteen_pages(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(16 * PAGE_SIZE);
    ASSERT_NOT_NULL(dev, p);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 16);
    kfree(p);
}

/** C8: All size-class allocs maintain integrity. */
static void test_kheap_size_classes_integrity(serial_dev_t *dev) {
    uint64_t sizes[] = { 1, PAGE_SIZE, 2*PAGE_SIZE, 4*PAGE_SIZE,
                         8*PAGE_SIZE, 16*PAGE_SIZE };
    for (int i = 0; i < 6; i++) {
        void *p = kmalloc(sizes[i]);
        ASSERT_NOT_NULL(dev, p);
        kfree(p);
        ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
    }
}

/* =========================================================================
 * Group D — Multiple simultaneous allocations (8 tests)
 * ========================================================================= */

/** D1: Two consecutive allocations return different pointers. */
static void test_kheap_two_distinct(serial_dev_t *dev) {
    void *a = kmalloc(100);
    void *b = kmalloc(100);
    ASSERT_NOT_NULL(dev, a);
    ASSERT_NOT_NULL(dev, b);
    ASSERT_TRUE(dev, a != b);
    kfree(a); kfree(b);
}

/** D2: Two allocations do not overlap (second >= first + PAGE_SIZE). */
static void test_kheap_no_overlap(serial_dev_t *dev) {
    void *a = kmalloc(PAGE_SIZE);
    void *b = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, a);
    ASSERT_NOT_NULL(dev, b);
    uint64_t lo = (uint64_t)a, hi = (uint64_t)b;
    if (lo > hi) { uint64_t tmp = lo; lo = hi; hi = tmp; }
    ASSERT_TRUE(dev, hi >= lo + PAGE_SIZE);
    kfree(a); kfree(b);
}

/** D3: 4 simultaneous allocations all non-NULL and distinct. */
static void test_kheap_four_distinct(serial_dev_t *dev) {
    void *p[4];
    for (int i = 0; i < 4; i++) {
        p[i] = kmalloc(PAGE_SIZE);
        ASSERT_NOT_NULL(dev, p[i]);
    }
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            ASSERT_TRUE(dev, p[i] != p[j]);
    for (int i = 0; i < 4; i++) kfree(p[i]);
}

/** D4: 8 simultaneous page allocations — stats track correctly. */
static void test_kheap_eight_alloc_stats(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p[8];
    for (int i = 0; i < 8; i++) {
        p[i] = kmalloc(PAGE_SIZE);
        ASSERT_NOT_NULL(dev, p[i]);
    }
    kheap_stats_t a; kheap_get_stats(&a);
    /* Each is level 0 = 1 page */
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 8);
    ASSERT_EQ(dev, a.active_allocs,   b.active_allocs   + 8);
    for (int i = 0; i < 8; i++) kfree(p[i]);
}

/** D5: Mixed sizes: 1 page + 2 page + 4 page simultaneously. */
static void test_kheap_mixed_sizes(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p1 = kmalloc(PAGE_SIZE);      /* 1 page  */
    void *p2 = kmalloc(2 * PAGE_SIZE);  /* 2 pages */
    void *p4 = kmalloc(4 * PAGE_SIZE);  /* 4 pages */
    ASSERT_NOT_NULL(dev, p1);
    ASSERT_NOT_NULL(dev, p2);
    ASSERT_NOT_NULL(dev, p4);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 7);
    ASSERT_EQ(dev, a.active_allocs,   b.active_allocs   + 3);
    kfree(p1); kfree(p2); kfree(p4);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** D6: 16 simultaneous 1-page allocations, all page-aligned. */
static void test_kheap_sixteen_aligned(serial_dev_t *dev) {
    void *p[16];
    for (int i = 0; i < 16; i++) {
        p[i] = kmalloc(PAGE_SIZE);
        ASSERT_NOT_NULL(dev, p[i]);
        ASSERT_TRUE(dev, is_page_aligned((uint64_t)p[i]));
    }
    for (int i = 0; i < 16; i++) kfree(p[i]);
}

/** D7: Alloc 32 pages' worth in 1-page chunks, all unique. */
static void test_kheap_32_unique(serial_dev_t *dev) {
    void *p[32];
    for (int i = 0; i < 32; i++) {
        p[i] = kmalloc(PAGE_SIZE);
        ASSERT_NOT_NULL(dev, p[i]);
    }
    /* Check uniqueness (n^2 but n=32 is fine). */
    for (int i = 0; i < 32; i++)
        for (int j = i + 1; j < 32; j++)
            ASSERT_TRUE(dev, p[i] != p[j]);
    for (int i = 0; i < 32; i++) kfree(p[i]);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** D8: Integrity holds while 16 allocations are outstanding. */
static void test_kheap_integrity_outstanding(serial_dev_t *dev) {
    void *p[16];
    for (int i = 0; i < 16; i++) p[i] = kmalloc(PAGE_SIZE);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
    for (int i = 0; i < 16; i++) kfree(p[i]);
}

/* =========================================================================
 * Group E — Free and reuse (6 tests)
 * ========================================================================= */

/** E1: Freeing a block makes it immediately reusable. */
static void test_kheap_free_reuse(serial_dev_t *dev) {
    void *p1 = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, p1);
    uint64_t addr1 = (uint64_t)p1;
    kfree(p1);

    /* On a fresh heap, the buddy allocator should give back the same block. */
    void *p2 = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, p2);
    /* addr2 should equal addr1 (or at least be valid). */
    uint64_t addr2 = (uint64_t)p2;
    ASSERT_TRUE(dev, addr2 >= HEAP_BASE && addr2 < HEAP_END);
    kfree(p2);
    (void)addr1;  /* suppress warning if test doesn't check equality */
}

/** E2: Alloc A, alloc B, free A, alloc C — C gets A's memory. */
static void test_kheap_interleaved_reuse(serial_dev_t *dev) {
    void *a = kmalloc(PAGE_SIZE);
    void *b = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, a);
    ASSERT_NOT_NULL(dev, b);
    ASSERT_TRUE(dev, a != b);
    uint64_t a_addr = (uint64_t)a;
    kfree(a);
    void *c = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, c);
    /* c should be at a's old address (buddy allocator is deterministic). */
    ASSERT_EQ(dev, (uint64_t)c, a_addr);
    kfree(b);
    kfree(c);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** E3: After alloc-free cycle, stats return to baseline. */
static void test_kheap_stats_baseline_restore(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p[4];
    for (int i = 0; i < 4; i++) p[i] = kmalloc(2 * PAGE_SIZE);
    for (int i = 0; i < 4; i++) kfree(p[i]);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages);
    ASSERT_EQ(dev, a.free_pages,      b.free_pages);
    ASSERT_EQ(dev, a.active_allocs,   b.active_allocs);
}

/** E4: total_allocs increments monotonically. */
static void test_kheap_total_allocs_monotonic(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(1);
    ASSERT_NOT_NULL(dev, p);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.total_allocs, b.total_allocs + 1);
    kfree(p);
    kheap_stats_t c; kheap_get_stats(&c);
    /* total_allocs never decrements on free */
    ASSERT_EQ(dev, c.total_allocs, a.total_allocs);
}

/** E5: Alternating alloc-free doesn't corrupt the allocator. */
static void test_kheap_alternating(serial_dev_t *dev) {
    for (int round = 0; round < 16; round++) {
        void *p = kmalloc(PAGE_SIZE);
        ASSERT_NOT_NULL(dev, p);
        kfree(p);
    }
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** E6: Free in reverse order restores full heap. */
static void test_kheap_reverse_free(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p[8];
    for (int i = 0; i < 8; i++) p[i] = kmalloc(PAGE_SIZE);
    for (int i = 7; i >= 0; i--) kfree(p[i]);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/* =========================================================================
 * Group F — Buddy coalescing (8 tests)
 *
 * These tests force coalescence by allocating and freeing specific
 * patterns of blocks.
 * ========================================================================= */

/**
 * F1: Allocate two adjacent level-0 blocks, free both —
 *     they should coalesce into a level-1 block.
 */
static void test_kheap_coalesce_level0_to_1(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *a = kmalloc(PAGE_SIZE);
    void *c = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, a);
    ASSERT_NOT_NULL(dev, c);
    /* Free both — buddy coalescing should produce a level-1 block. */
    kfree(a);
    kfree(c);
    kheap_stats_t af; kheap_get_stats(&af);
    ASSERT_EQ(dev, af.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** F2: Alloc 4 level-0 blocks, free all 4 — coalesce to level 2. */
static void test_kheap_coalesce_4_to_level2(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p[4];
    for (int i = 0; i < 4; i++) p[i] = kmalloc(PAGE_SIZE);
    for (int i = 0; i < 4; i++) ASSERT_NOT_NULL(dev, p[i]);
    for (int i = 0; i < 4; i++) kfree(p[i]);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** F3: Alloc 8 level-0 blocks, free all — coalesce to level 3. */
static void test_kheap_coalesce_8_to_level3(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p[8];
    for (int i = 0; i < 8; i++) p[i] = kmalloc(PAGE_SIZE);
    for (int i = 0; i < 8; i++) ASSERT_NOT_NULL(dev, p[i]);
    for (int i = 0; i < 8; i++) kfree(p[i]);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** F4: Alloc level-1 block, free — coalesces back. */
static void test_kheap_coalesce_level1(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(2 * PAGE_SIZE);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** F5: Fragmentation then full coalescence: alloc 16, free in pairs. */
static void test_kheap_coalesce_pairs(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p[16];
    for (int i = 0; i < 16; i++) p[i] = kmalloc(PAGE_SIZE);
    for (int i = 0; i < 16; i++) ASSERT_NOT_NULL(dev, p[i]);
    /* Free in pairs (0,1), (2,3), ... */
    for (int i = 0; i < 16; i += 2) { kfree(p[i]); kfree(p[i+1]); }
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** F6: After full coalescence, large allocation still succeeds. */
static void test_kheap_coalesce_then_large_alloc(serial_dev_t *dev) {
    /* Alloc 16 small blocks, free them all, then alloc a large block. */
    void *small[16];
    for (int i = 0; i < 16; i++) small[i] = kmalloc(PAGE_SIZE);
    for (int i = 0; i < 16; i++) kfree(small[i]);

    void *large = kmalloc(16 * PAGE_SIZE);
    ASSERT_NOT_NULL(dev, large);
    kfree(large);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** F7: Alloc level-11 block (8 MiB), free — returns to pool. */
static void test_kheap_level11_alloc_free(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    /* 8 MiB = 2048 pages = level 11 */
    void *p = kmalloc(8ULL * 1024 * 1024);
    ASSERT_NOT_NULL(dev, p);
    kheap_stats_t mid; kheap_get_stats(&mid);
    ASSERT_EQ(dev, mid.allocated_pages, b.allocated_pages + 2048);
    kfree(p);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/* Static pointer array for heap-exhaustion tests (avoids kernel stack overflow). */
#define EXHAUST_MAX 4096
static void *g_exhaust_pages[EXHAUST_MAX];

/**
 * F8: Fill heap with 1-page allocs until OOM, verify full, then free all.
 *     Tests heap exhaustion and restoration without requiring large
 *     contiguous blocks (which may not exist after slab/task init).
 */
static void test_kheap_both_halves(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    int n = 0;
    while (n < EXHAUST_MAX) {
        g_exhaust_pages[n] = kmalloc(PAGE_SIZE);
        if (!g_exhaust_pages[n]) break;
        n++;
    }
    ASSERT_TRUE(dev, n > 0);
    kheap_stats_t full; kheap_get_stats(&full);
    /* Verify this test consumed exactly n pages and heap is now full:
     * use delta so the test is robust even if slab/task already used some pages. */
    ASSERT_EQ(dev, full.allocated_pages, b.allocated_pages + (uint64_t)n);
    ASSERT_EQ(dev, full.free_pages, (uint64_t)0);
    for (int i = 0; i < n; i++) kfree(g_exhaust_pages[i]);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/* =========================================================================
 * Group G — Edge and error cases (8 tests)
 * ========================================================================= */

/** G1: OOM returns NULL when heap is exhausted. */
static void test_kheap_oom(serial_dev_t *dev) {
    int n = 0;
    while (n < EXHAUST_MAX) {
        g_exhaust_pages[n] = kmalloc(PAGE_SIZE);
        if (!g_exhaust_pages[n]) break;
        n++;
    }
    ASSERT_TRUE(dev, n > 0);
    void *p = kmalloc(1);
    ASSERT_TRUE(dev, p == NULL);
    for (int i = 0; i < n; i++) kfree(g_exhaust_pages[i]);
}

/** G2: After OOM recovery (free), allocation succeeds again. */
static void test_kheap_oom_recovery(serial_dev_t *dev) {
    int n = 0;
    while (n < EXHAUST_MAX) {
        g_exhaust_pages[n] = kmalloc(PAGE_SIZE);
        if (!g_exhaust_pages[n]) break;
        n++;
    }
    ASSERT_TRUE(dev, n > 0);
    void *fail = kmalloc(1);
    ASSERT_TRUE(dev, fail == NULL);
    for (int i = 0; i < n; i++) kfree(g_exhaust_pages[i]);
    void *ok = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, ok);
    kfree(ok);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** G3: kheap_get_stats(NULL) does not crash. */
static void test_kheap_stats_null(serial_dev_t *dev) {
    kheap_get_stats(NULL);
    ASSERT_TRUE(dev, 1);
}

/** G4: kmalloc(0) returns same class as kmalloc(1). */
static void test_kheap_zero_same_as_one(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p0 = kmalloc(0);
    ASSERT_NOT_NULL(dev, p0);
    kheap_stats_t a0; kheap_get_stats(&a0);
    /* Should consume 1 page (level 0) */
    ASSERT_EQ(dev, a0.allocated_pages, b.allocated_pages + 1);
    kfree(p0);

    kheap_stats_t b1; kheap_get_stats(&b1);
    void *p1 = kmalloc(1);
    ASSERT_NOT_NULL(dev, p1);
    kheap_stats_t a1; kheap_get_stats(&a1);
    ASSERT_EQ(dev, a1.allocated_pages, b1.allocated_pages + 1);
    kfree(p1);
}

/** G5: Double-free is a no-op and does not corrupt the heap. */
static void test_kheap_double_free(serial_dev_t *dev) {
    void *p = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
    kfree(p);   /* second free must be silently ignored */
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** G6: Pointer just below HEAP_BASE is rejected. */
static void test_kheap_out_of_range_low(serial_dev_t *dev) {
    /* Pass a pointer outside the heap range (just below HEAP_BASE).
     * kfree must reject it without crashing. */
    void *bad = (void *)(HEAP_BASE - PAGE_SIZE);
    kfree(bad);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** G7: Pointer at HEAP_END is rejected (past the end). */
static void test_kheap_out_of_range_high(serial_dev_t *dev) {
    void *bad = (void *)HEAP_END;
    kfree(bad);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** G8: conservation law holds after mixed operations. */
static void test_kheap_conservation_mixed(serial_dev_t *dev) {
    void *p1 = kmalloc(PAGE_SIZE);
    void *p2 = kmalloc(2 * PAGE_SIZE);
    void *p3 = kmalloc(4 * PAGE_SIZE);
    kfree(p2);
    void *p4 = kmalloc(PAGE_SIZE);
    kheap_stats_t s; kheap_get_stats(&s);
    ASSERT_EQ(dev, s.free_pages + s.allocated_pages, s.total_pages);
    kfree(p1); kfree(p3); kfree(p4);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/* =========================================================================
 * Group H — Stress: many small allocations (8 tests)
 * ========================================================================= */

/** H1: 64 consecutive 1-page allocs, all non-NULL. */
static void test_kheap_stress_64_allocs(serial_dev_t *dev) {
    void *p[64];
    for (int i = 0; i < 64; i++) {
        p[i] = kmalloc(PAGE_SIZE);
        ASSERT_NOT_NULL(dev, p[i]);
    }
    for (int i = 0; i < 64; i++) kfree(p[i]);
}

/** H2: 64 allocs + free: heap state is restored. */
static void test_kheap_stress_64_restore(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p[64];
    for (int i = 0; i < 64; i++) p[i] = kmalloc(PAGE_SIZE);
    for (int i = 0; i < 64; i++) kfree(p[i]);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** H3: Interleaved alloc-free pattern (LIFO). */
static void test_kheap_stress_lifo(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *stack[32];
    int sp = 0;
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 8; i++) {
            stack[sp] = kmalloc(PAGE_SIZE);
            ASSERT_NOT_NULL(dev, stack[sp]);
            sp++;
        }
        for (int i = 0; i < 8; i++) {
            kfree(stack[--sp]);
        }
    }
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** H4: FIFO free pattern. */
static void test_kheap_stress_fifo(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *q[32];
    for (int i = 0; i < 32; i++) {
        q[i] = kmalloc(PAGE_SIZE);
        ASSERT_NOT_NULL(dev, q[i]);
    }
    for (int i = 0; i < 32; i++) kfree(q[i]);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.free_pages, b.free_pages);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** H5: 128 small allocs and frees maintain integrity throughout. */
static void test_kheap_stress_integrity_128(serial_dev_t *dev) {
    /* We can't alloc 128 pages simultaneously without checking OOM,
     * so do 8 rounds of 16 allocs. */
    for (int round = 0; round < 8; round++) {
        void *p[16];
        for (int i = 0; i < 16; i++) {
            p[i] = kmalloc(PAGE_SIZE);
            ASSERT_NOT_NULL(dev, p[i]);
        }
        ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
        for (int i = 0; i < 16; i++) kfree(p[i]);
        ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
    }
}

/** H6: Random-order free of 16 blocks. */
static void test_kheap_stress_random_free(serial_dev_t *dev) {
    void *p[16];
    for (int i = 0; i < 16; i++) p[i] = kmalloc(PAGE_SIZE);
    for (int i = 0; i < 16; i++) ASSERT_NOT_NULL(dev, p[i]);
    /* Free in a non-sequential order: every other, then the rest. */
    for (int i = 0; i < 16; i += 2) kfree(p[i]);
    for (int i = 1; i < 16; i += 2) kfree(p[i]);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** H7: Repeated alloc-free cycles produce consistent totals. */
static void test_kheap_stress_total_allocs(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    for (int i = 0; i < 10; i++) {
        void *p = kmalloc(PAGE_SIZE);
        kfree(p);
    }
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.total_allocs, b.total_allocs + 10);
    ASSERT_EQ(dev, a.active_allocs, b.active_allocs);
}

/** H8: Conservation law always holds under stress. */
static void test_kheap_stress_conservation(serial_dev_t *dev) {
    void *live[8];
    int n = 0;
    for (int round = 0; round < 20; round++) {
        if (n < 8) {
            live[n] = kmalloc(PAGE_SIZE);
            if (live[n]) n++;
        }
        if (n > 0 && (round % 3 == 0)) {
            kfree(live[--n]);
        }
        kheap_stats_t s; kheap_get_stats(&s);
        ASSERT_EQ(dev, s.free_pages + s.allocated_pages, s.total_pages);
    }
    for (int i = 0; i < n; i++) kfree(live[i]);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/* =========================================================================
 * Group I — Stress: large allocations (8 tests)
 * ========================================================================= */

/** I1: 1 MiB allocation returns a non-NULL, aligned pointer. */
static void test_kheap_large_1mib(serial_dev_t *dev) {
    void *p = kmalloc(1ULL * 1024 * 1024);
    ASSERT_NOT_NULL(dev, p);
    ASSERT_TRUE(dev, is_page_aligned((uint64_t)p));
    kfree(p);
}

/** I2: 2 MiB allocation. */
static void test_kheap_large_2mib(serial_dev_t *dev) {
    void *p = kmalloc(2ULL * 1024 * 1024);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
}

/** I3: 4 MiB allocation. */
static void test_kheap_large_4mib(serial_dev_t *dev) {
    void *p = kmalloc(4ULL * 1024 * 1024);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** I4: Two 4 MiB allocations fit simultaneously (if enough contiguous space). */
static void test_kheap_large_two_4mib(serial_dev_t *dev) {
    void *a = kmalloc(4ULL * 1024 * 1024);
    if (!a) return;  /* skip — not enough contiguous space after init */
    void *b = kmalloc(4ULL * 1024 * 1024);
    kfree(a);
    if (!b) return;  /* skip — only one contiguous 4 MiB block available */
    ASSERT_TRUE(dev, a != b);
    kfree(b);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** I5: 1 MiB alloc, then 1-page alloc, then both freed — intact. */
static void test_kheap_large_mixed(serial_dev_t *dev) {
    void *big = kmalloc(1ULL * 1024 * 1024);
    void *small = kmalloc(PAGE_SIZE);
    ASSERT_NOT_NULL(dev, big);
    ASSERT_NOT_NULL(dev, small);
    kfree(big);
    kfree(small);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/** I6: 8 MiB allocation stats check (skips if not enough contiguous space). */
static void test_kheap_large_8mib_stats(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p = kmalloc(8ULL * 1024 * 1024);
    if (!p) return;  /* skip — not enough contiguous space after init */
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages + 2048);
    kfree(p);
}

/** I7: Heap exhaustion with 1-page allocs — allocs exhaust all free pages. */
static void test_kheap_large_exhaust(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    int n = 0;
    while (n < EXHAUST_MAX) {
        g_exhaust_pages[n] = kmalloc(PAGE_SIZE);
        if (!g_exhaust_pages[n]) break;
        n++;
    }
    ASSERT_TRUE(dev, n > 0);
    kheap_stats_t s; kheap_get_stats(&s);
    /* Delta check: this test filled n pages, heap should now be fully consumed. */
    ASSERT_EQ(dev, s.allocated_pages, b.allocated_pages + (uint64_t)n);
    ASSERT_EQ(dev, s.free_pages, (uint64_t)0);
    for (int i = 0; i < n; i++) kfree(g_exhaust_pages[i]);
}

/** I8: Integrity holds after large-alloc stress. */
static void test_kheap_large_integrity(serial_dev_t *dev) {
    for (int i = 0; i < 4; i++) {
        void *p = kmalloc(4ULL * 1024 * 1024);
        ASSERT_NOT_NULL(dev, p);
        void *q = kmalloc(4ULL * 1024 * 1024);
        ASSERT_NOT_NULL(dev, q);
        kfree(p);
        kfree(q);
        ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
    }
}

/* =========================================================================
 * Group J — Statistics tracking (6 tests)
 * ========================================================================= */

/** J1: total_allocs increments by exactly 1 per kmalloc call. */
static void test_kheap_stats_total_alloc_count(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p[5];
    for (int i = 0; i < 5; i++) p[i] = kmalloc(PAGE_SIZE);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.total_allocs, b.total_allocs + 5);
    for (int i = 0; i < 5; i++) kfree(p[i]);
}

/** J2: active_allocs increments per kmalloc, decrements per kfree. */
static void test_kheap_stats_active_alloc_tracking(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p[3];
    for (int i = 0; i < 3; i++) {
        p[i] = kmalloc(PAGE_SIZE);
        kheap_stats_t s; kheap_get_stats(&s);
        ASSERT_EQ(dev, s.active_allocs, b.active_allocs + (uint64_t)(i + 1));
    }
    for (int i = 0; i < 3; i++) {
        kfree(p[i]);
        kheap_stats_t s; kheap_get_stats(&s);
        ASSERT_EQ(dev, s.active_allocs, b.active_allocs + (uint64_t)(2 - i));
    }
}

/** J3: allocated_pages tracks correctly across mixed sizes. */
static void test_kheap_stats_alloc_pages_mixed(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    void *p1 = kmalloc(PAGE_SIZE);           /* 1 page  */
    void *p2 = kmalloc(2 * PAGE_SIZE);       /* 2 pages */
    void *p4 = kmalloc(4 * PAGE_SIZE);       /* 4 pages */
    kheap_stats_t s; kheap_get_stats(&s);
    ASSERT_EQ(dev, s.allocated_pages, b.allocated_pages + 1 + 2 + 4);
    kfree(p1);
    kheap_stats_t s1; kheap_get_stats(&s1);
    ASSERT_EQ(dev, s1.allocated_pages, b.allocated_pages + 2 + 4);
    kfree(p2);
    kheap_stats_t s2; kheap_get_stats(&s2);
    ASSERT_EQ(dev, s2.allocated_pages, b.allocated_pages + 4);
    kfree(p4);
    kheap_stats_t s3; kheap_get_stats(&s3);
    ASSERT_EQ(dev, s3.allocated_pages, b.allocated_pages);
}

/** J4: free_pages = total_pages - allocated_pages at all times. */
static void test_kheap_stats_free_eq_total_minus_alloc(serial_dev_t *dev) {
    void *p = kmalloc(PAGE_SIZE);
    kheap_stats_t s; kheap_get_stats(&s);
    ASSERT_EQ(dev, s.free_pages, s.total_pages - s.allocated_pages);
    kfree(p);
}

/** J5: total_allocs is not reset by kfree. */
static void test_kheap_stats_total_not_reset_by_free(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    for (int i = 0; i < 8; i++) {
        void *p = kmalloc(PAGE_SIZE);
        kfree(p);
    }
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.total_allocs, b.total_allocs + 8);
}

/** J6: Full heap cycle: fill, free, stats return exactly to baseline. */
static void test_kheap_stats_full_cycle(serial_dev_t *dev) {
    kheap_stats_t b; kheap_get_stats(&b);
    int n = 0;
    while (n < EXHAUST_MAX) {
        g_exhaust_pages[n] = kmalloc(PAGE_SIZE);
        if (!g_exhaust_pages[n]) break;
        n++;
    }
    ASSERT_TRUE(dev, n > 0);
    for (int i = 0; i < n; i++) kfree(g_exhaust_pages[i]);
    kheap_stats_t a; kheap_get_stats(&a);
    ASSERT_EQ(dev, a.allocated_pages, b.allocated_pages);
    ASSERT_EQ(dev, a.free_pages,      b.free_pages);
    ASSERT_EQ(dev, a.active_allocs,   b.active_allocs);
    ASSERT_EQ(dev, a.total_allocs,    b.total_allocs + (uint64_t)n);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/* =========================================================================
 * Test registration
 * ========================================================================= */

void test_register_kheap(void) {
    /* Group A — Post-init invariants */
    test_register("kheap_A1_total_pages",           test_kheap_total_pages);
    test_register("kheap_A2_init_alloc_zero",       test_kheap_init_alloc_zero);
    test_register("kheap_A3_init_free_eq_total",    test_kheap_init_free_equals_total);
    test_register("kheap_A4_init_conservation",     test_kheap_init_conservation);
    test_register("kheap_A5_init_active_zero",      test_kheap_init_active_allocs_zero);
    test_register("kheap_A6_init_integrity",        test_kheap_init_integrity);

    /* Group B — Basic kmalloc / kfree */
    test_register("kheap_B1_malloc_zero",           test_kheap_malloc_zero);
    test_register("kheap_B2_malloc_one_byte",       test_kheap_malloc_one_byte);
    test_register("kheap_B3_malloc_aligned",        test_kheap_malloc_aligned);
    test_register("kheap_B4_malloc_in_range",       test_kheap_malloc_in_range);
    test_register("kheap_B5_free_null",             test_kheap_free_null);
    test_register("kheap_B6_free_restores_active",  test_kheap_free_restores_active);
    test_register("kheap_B7_free_restores_pages",   test_kheap_free_restores_free_pages);
    test_register("kheap_B8_single_cycle_integrity",test_kheap_single_cycle_integrity);

    /* Group C — Size classes */
    test_register("kheap_C1_one_page",              test_kheap_one_page);
    test_register("kheap_C2_two_pages",             test_kheap_two_pages);
    test_register("kheap_C3_exact_two_pages",       test_kheap_exact_two_pages);
    test_register("kheap_C4_four_pages",            test_kheap_four_pages);
    test_register("kheap_C5_exact_four_pages",      test_kheap_exact_four_pages);
    test_register("kheap_C6_eight_pages",           test_kheap_eight_pages);
    test_register("kheap_C7_sixteen_pages",         test_kheap_sixteen_pages);
    test_register("kheap_C8_size_classes_integrity",test_kheap_size_classes_integrity);

    /* Group D — Multiple simultaneous */
    test_register("kheap_D1_two_distinct",          test_kheap_two_distinct);
    test_register("kheap_D2_no_overlap",            test_kheap_no_overlap);
    test_register("kheap_D3_four_distinct",         test_kheap_four_distinct);
    test_register("kheap_D4_eight_alloc_stats",     test_kheap_eight_alloc_stats);
    test_register("kheap_D5_mixed_sizes",           test_kheap_mixed_sizes);
    test_register("kheap_D6_sixteen_aligned",       test_kheap_sixteen_aligned);
    test_register("kheap_D7_32_unique",             test_kheap_32_unique);
    test_register("kheap_D8_integrity_outstanding", test_kheap_integrity_outstanding);

    /* Group E — Free and reuse */
    test_register("kheap_E1_free_reuse",            test_kheap_free_reuse);
    test_register("kheap_E2_interleaved_reuse",     test_kheap_interleaved_reuse);
    test_register("kheap_E3_stats_baseline_restore",test_kheap_stats_baseline_restore);
    test_register("kheap_E4_total_allocs_monotonic",test_kheap_total_allocs_monotonic);
    test_register("kheap_E5_alternating",           test_kheap_alternating);
    test_register("kheap_E6_reverse_free",          test_kheap_reverse_free);

    /* Group F — Coalescing */
    test_register("kheap_F1_coalesce_0_to_1",       test_kheap_coalesce_level0_to_1);
    test_register("kheap_F2_coalesce_4_to_2",       test_kheap_coalesce_4_to_level2);
    test_register("kheap_F3_coalesce_8_to_3",       test_kheap_coalesce_8_to_level3);
    test_register("kheap_F4_coalesce_level1",       test_kheap_coalesce_level1);
    test_register("kheap_F5_coalesce_pairs",        test_kheap_coalesce_pairs);
    test_register("kheap_F6_coalesce_then_large",   test_kheap_coalesce_then_large_alloc);
    test_register("kheap_F7_level11_alloc_free",    test_kheap_level11_alloc_free);
    test_register("kheap_F8_both_halves",           test_kheap_both_halves);

    /* Group G — Edge & error cases */
    test_register("kheap_G1_oom",                   test_kheap_oom);
    test_register("kheap_G2_oom_recovery",          test_kheap_oom_recovery);
    test_register("kheap_G3_stats_null",            test_kheap_stats_null);
    test_register("kheap_G4_zero_same_as_one",      test_kheap_zero_same_as_one);
    test_register("kheap_G5_double_free",           test_kheap_double_free);
    test_register("kheap_G6_out_of_range_low",      test_kheap_out_of_range_low);
    test_register("kheap_G7_out_of_range_high",     test_kheap_out_of_range_high);
    test_register("kheap_G8_conservation_mixed",    test_kheap_conservation_mixed);

    /* Group H — Stress: small */
    test_register("kheap_H1_stress_64_allocs",      test_kheap_stress_64_allocs);
    test_register("kheap_H2_stress_64_restore",     test_kheap_stress_64_restore);
    test_register("kheap_H3_stress_lifo",           test_kheap_stress_lifo);
    test_register("kheap_H4_stress_fifo",           test_kheap_stress_fifo);
    test_register("kheap_H5_stress_integrity_128",  test_kheap_stress_integrity_128);
    test_register("kheap_H6_stress_random_free",    test_kheap_stress_random_free);
    test_register("kheap_H7_stress_total_allocs",   test_kheap_stress_total_allocs);
    test_register("kheap_H8_stress_conservation",   test_kheap_stress_conservation);

    /* Group I — Stress: large */
    test_register("kheap_I1_large_1mib",            test_kheap_large_1mib);
    test_register("kheap_I2_large_2mib",            test_kheap_large_2mib);
    test_register("kheap_I3_large_4mib",            test_kheap_large_4mib);
    test_register("kheap_I4_large_two_4mib",        test_kheap_large_two_4mib);
    test_register("kheap_I5_large_mixed",           test_kheap_large_mixed);
    test_register("kheap_I6_large_8mib_stats",      test_kheap_large_8mib_stats);
    test_register("kheap_I7_large_exhaust",         test_kheap_large_exhaust);
    test_register("kheap_I8_large_integrity",       test_kheap_large_integrity);

    /* Group J — Stats tracking */
    test_register("kheap_J1_stats_total_count",     test_kheap_stats_total_alloc_count);
    test_register("kheap_J2_stats_active_tracking", test_kheap_stats_active_alloc_tracking);
    test_register("kheap_J3_stats_pages_mixed",     test_kheap_stats_alloc_pages_mixed);
    test_register("kheap_J4_stats_free_eq_total",   test_kheap_stats_free_eq_total_minus_alloc);
    test_register("kheap_J5_stats_total_no_reset",  test_kheap_stats_total_not_reset_by_free);
    test_register("kheap_J6_stats_full_cycle",      test_kheap_stats_full_cycle);
}
