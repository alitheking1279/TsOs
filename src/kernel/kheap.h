/**
 * @file kheap.h
 * @brief Kernel Heap — page-granularity buddy allocator.
 *
 * The kernel heap provides dynamic memory allocation for the kernel
 * using a buddy allocator over a fixed virtual address range
 * ([HEAP_BASE .. HEAP_END) = 16 MiB, 4096 pages).
 *
 * All heap pages are demand-paged: no physical frames are allocated
 * until the first access.  The page fault handler in main.c allocates
 * a frame and maps it when a demand page is touched.
 *
 * Buddy allocator design
 * ======================
 * The buddy system operates over 4 KiB virtual pages at 12 power-of-2
 * levels:
 *   Level 0:  1 page  (4 KiB)
 *   Level 1:  2 pages (8 KiB)
 *   Level 2:  4 pages (16 KiB)
 *   ...
 *   Level 11: 2048 pages (8 MiB)
 *
 * Each level k has a singly-linked free list.  On allocation, the
 * smallest available block is split down to the target level.  On
 * deallocation, the block is returned to its level and coalesced
 * with its buddy if both are free.
 *
 * Buddy address calculation:
 *   block_addr ^ (PAGE_SIZE << level)
 *   (XOR the (level+12)-th bit of the virtual address)
 *
 * Safety
 * ======
 *   - kmalloc(0) returns a valid 1-page pointer (matches POSIX).
 *   - kfree(NULL) is a no-op (matches standard C free).
 *   - Double-free is detected and logged (no-op, returns silently).
 *   - Allocation beyond heap capacity returns NULL.
 */

#ifndef KERNEL_KHEAP_H
#define KERNEL_KHEAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Kheap Statistics
 * ========================================================================= */

typedef struct {
    uint64_t total_pages;      /**< Total heap pages (4096). */
    uint64_t allocated_pages;  /**< Currently allocated pages. */
    uint64_t free_pages;       /**< Currently free pages. */
    uint64_t total_allocs;     /**< Lifetime allocation count. */
    uint64_t active_allocs;    /**< Current live allocations. */
} kheap_stats_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the kernel heap buddy allocator.
 *
 * Must be called after vmm_init() and after the identity map is
 * removed.  Sets up the buddy free lists — no physical frames are
 * allocated until the first kmalloc touch triggers a page fault.
 *
 * @param serial_dev  Serial device for logging.  May be NULL.
 */
void kheap_init(void *serial_dev);

/**
 * @brief Allocate memory from the kernel heap.
 *
 * Returns a page-aligned virtual address within [HEAP_BASE, HEAP_END).
 * The returned memory is NOT zeroed.  At least 1 page (4096 bytes) is
 * allocated regardless of `size`.
 *
 * @param size  Number of bytes to allocate (0 or more).
 * @return Virtual address of allocated block, or NULL on OOM.
 */
void *kmalloc(uint64_t size);

/**
 * @brief Free a previous kmalloc allocation.
 *
 * Returns the allocated pages to the buddy free list and coalesces
 * with buddies where possible.  Physical frames are NOT freed immediately
 * (they remain mapped for reuse).  Pass NULL safely (no-op).
 *
 * @param ptr  Pointer returned by kmalloc, or NULL.
 */
void kfree(void *ptr);

/**
 * @brief Fill a kheap_stats_t with current allocator statistics.
 *
 * @param out  Destination struct.  Must not be NULL.
 */
void kheap_get_stats(kheap_stats_t *out);

/**
 * @brief Get the heap statistics snapshot from right after kheap_init().
 *
 * Group A tests compare current stats against this baseline instead of
 * hardcoded zero values, since slab/task/scheduler allocations occur
 * between kheap_init() and test execution.
 *
 * @param out  Destination struct.  Must not be NULL.
 */
void kheap_get_init_stats(kheap_stats_t *out);

/**
 * @brief Verify internal buddy-allocator invariants.
 *
 * Walks all free lists and checks:
 *   - Every free block's page index is in range.
 *   - Every free block's g_block_level tag matches its list level.
 *   - No page appears on more than one free list.
 *   - Interior pages of every free block are correctly tagged.
 *   - Total free pages from lists equals (total - allocated).
 *
 * Logs all violations to the serial port.  Intended for use by the
 * test suite and during kernel development.
 *
 * @return 1 if all invariants hold, 0 if any violation is detected.
 */
int kheap_verify_integrity(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_KHEAP_H */
