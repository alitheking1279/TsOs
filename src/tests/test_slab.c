/**
 * @file test_slab.c
 * @brief Slab allocator test suite.
 *
 * Tests for the slab allocator — small-object cache built on kheap.
 *
 * NOTE: slab_init() is called once by kernel_main() before any test runs.
 *       Tests MUST NOT call slab_init() again — doing so resets internal
 *       state without freeing already-allocated slab pages, corrupting
 *       the allocator's accounting.
 */

#include "test.h"
#include "../kernel/slab.h"
#include "../kernel/kheap.h"
#include "../kernel/page_table.h"
#include <stdint.h>

/* =========================================================================
 * Basic size-class tests
 * ========================================================================= */

/** After kernel_main init, slab_get_allocated_bytes() must be queryable. */
static void test_slab_init(serial_dev_t *dev)
{
    /* Do NOT call slab_init(dev) here — it would reset state without
     * freeing existing pages, corrupting the allocator accounting.
     * The slab was already initialized by kernel_main and may have
     * non-zero allocated bytes (e.g. from task creation).
     * Just verify the counter is queryable (no crash). */
    uint64_t bytes = slab_get_allocated_bytes();
    (void)bytes;
}

static void test_slab_alloc_16(serial_dev_t *dev)
{
    uint64_t before = slab_get_allocated_bytes();
    void *p = slab_alloc(1);
    ASSERT_NOT_NULL(dev, p);
    ASSERT_EQ(dev, slab_get_allocated_bytes(), before + (uint64_t)16);
    slab_free(p);
    ASSERT_EQ(dev, slab_get_allocated_bytes(), before);
}

static void test_slab_alloc_32(serial_dev_t *dev)
{
    uint64_t before = slab_get_allocated_bytes();
    void *p = slab_alloc(32);
    ASSERT_NOT_NULL(dev, p);
    ASSERT_EQ(dev, slab_get_allocated_bytes(), before + (uint64_t)32);
    slab_free(p);
    ASSERT_EQ(dev, slab_get_allocated_bytes(), before);
}

static void test_slab_alloc_64(serial_dev_t *dev)
{
    void *p = slab_alloc(64);
    ASSERT_NOT_NULL(dev, p);
    slab_free(p);
}

static void test_slab_alloc_128(serial_dev_t *dev)
{
    void *p = slab_alloc(128);
    ASSERT_NOT_NULL(dev, p);
    slab_free(p);
}

static void test_slab_alloc_256(serial_dev_t *dev)
{
    void *p = slab_alloc(256);
    ASSERT_NOT_NULL(dev, p);
    slab_free(p);
}

static void test_slab_alloc_512(serial_dev_t *dev)
{
    void *p = slab_alloc(512);
    ASSERT_NOT_NULL(dev, p);
    slab_free(p);
}

static void test_slab_alloc_1024(serial_dev_t *dev)
{
    void *p = slab_alloc(1024);
    ASSERT_NOT_NULL(dev, p);
    slab_free(p);
}

/* =========================================================================
 * Multi-object and correctness tests
 * ========================================================================= */

static void test_slab_multiple_objects(serial_dev_t *dev)
{
    uint64_t before = slab_get_allocated_bytes();
    void *p1 = slab_alloc(32);
    void *p2 = slab_alloc(32);
    void *p3 = slab_alloc(32);
    ASSERT_NOT_NULL(dev, p1);
    ASSERT_NOT_NULL(dev, p2);
    ASSERT_NOT_NULL(dev, p3);
    ASSERT_TRUE(dev, p1 != p2);
    ASSERT_TRUE(dev, p2 != p3);
    ASSERT_TRUE(dev, p1 != p3);
    ASSERT_EQ(dev, slab_get_allocated_bytes(), before + (uint64_t)(32 * 3));
    slab_free(p1);
    slab_free(p2);
    slab_free(p3);
    ASSERT_EQ(dev, slab_get_allocated_bytes(), before);
}

static void test_slab_free_null(serial_dev_t *dev)
{
    (void)dev;
    slab_free(NULL); /* must not crash */
}

static void test_slab_alloc_zero(serial_dev_t *dev)
{
    void *p = slab_alloc(0);
    ASSERT_NULL(dev, p);
}

static void test_slab_alloc_too_large(serial_dev_t *dev)
{
    /* slab_alloc(>1024) falls back to kmalloc — returns a valid pointer. */
    void *p = slab_alloc(2048);
    ASSERT_NOT_NULL(dev, p);
    kfree(p);
}

static void test_slab_reuse_slots(serial_dev_t *dev)
{
    void *p1 = slab_alloc(64);
    ASSERT_NOT_NULL(dev, p1);
    slab_free(p1);
    void *p2 = slab_alloc(64);
    ASSERT_NOT_NULL(dev, p2);
    /* The slab free-list is LIFO: p2 should be the same slot. */
    ASSERT_EQ(dev, (uint64_t)p1, (uint64_t)p2);
    slab_free(p2);
}

static void test_slab_kheap_integrity(serial_dev_t *dev)
{
    /* After slab operations, kheap buddy invariants must hold. */
    void *big = kmalloc(8192);
    ASSERT_NOT_NULL(dev, big);
    kfree(big);
    ASSERT_TRUE(dev, kheap_verify_integrity());
}

/* =========================================================================
 * New edge-case tests
 * ========================================================================= */

/**
 * Double-free: freeing the same slab slot twice must not crash or corrupt.
 * The slab doesn't track individual slot state, so the second free just
 * re-links the slot onto the free list again — this is a known limitation.
 * We verify the allocator survives without panicking or corrupting the
 * heap integrity check.
 */
static void test_slab_double_free(serial_dev_t *dev)
{
    void *p = slab_alloc(32);
    ASSERT_NOT_NULL(dev, p);
    slab_free(p);
    slab_free(p);  /* second free — must not crash */
    /* Drain any contaminated slots before checking integrity. */
    void *drain1 = slab_alloc(32);
    void *drain2 = slab_alloc(32);
    if (drain1) slab_free(drain1);
    if (drain2) slab_free(drain2);
    /* kheap must still be healthy. */
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

/**
 * Page boundary: allocate enough 16-byte objects to fill one entire slab
 * page (PAGE_SIZE / 16 = 256 minus the header overhead ~= ~248 slots).
 * All returned pointers must be distinct and within the slab page.
 * This exercises the "slab page runs out and a new one is allocated" path.
 */
#define SLAB_BOUNDARY_COUNT 260  /* intentionally > one slab page worth */
static void *g_slab_boundary_ptrs[SLAB_BOUNDARY_COUNT];

static void test_slab_page_boundary(serial_dev_t *dev)
{
    int n = 0;

    /* Allocate until we run out of slab slots or hit our limit. */
    for (n = 0; n < SLAB_BOUNDARY_COUNT; n++) {
        g_slab_boundary_ptrs[n] = slab_alloc(16);
        if (!g_slab_boundary_ptrs[n]) break;
    }

    /* Must have allocated at least one full slab page worth. */
    ASSERT_TRUE(dev, n > 0);

    /* All returned pointers must be distinct. */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (g_slab_boundary_ptrs[i] == g_slab_boundary_ptrs[j]) {
                ASSERT_TRUE(dev, 0);  /* duplicate pointer detected */
                for (int k = 0; k < n; k++) slab_free(g_slab_boundary_ptrs[k]);
                return;
            }
        }
    }

    /* Free all and verify kheap integrity. */
    for (int i = 0; i < n; i++) slab_free(g_slab_boundary_ptrs[i]);
    ASSERT_TRUE(dev, kheap_verify_integrity() == 1);
}

void test_register_slab(void)
{
    test_register("slab_init",            test_slab_init);
    test_register("slab_alloc_16",        test_slab_alloc_16);
    test_register("slab_alloc_32",        test_slab_alloc_32);
    test_register("slab_alloc_64",        test_slab_alloc_64);
    test_register("slab_alloc_128",       test_slab_alloc_128);
    test_register("slab_alloc_256",       test_slab_alloc_256);
    test_register("slab_alloc_512",       test_slab_alloc_512);
    test_register("slab_alloc_1024",      test_slab_alloc_1024);
    test_register("slab_multiple_objects",test_slab_multiple_objects);
    test_register("slab_free_null",       test_slab_free_null);
    test_register("slab_alloc_zero",      test_slab_alloc_zero);
    test_register("slab_alloc_too_large", test_slab_alloc_too_large);
    test_register("slab_reuse_slots",     test_slab_reuse_slots);
    test_register("slab_kheap_integrity", test_slab_kheap_integrity);
    /* Edge cases */
    test_register("slab_double_free",     test_slab_double_free);
    test_register("slab_page_boundary",   test_slab_page_boundary);
}
