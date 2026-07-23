#ifndef KERNEL_PMM_BUDDY_H
#define KERNEL_PMM_BUDDY_H

#include <stdint.h>

/* PMM buddy layer — efficient contiguous frame allocation.
 *
 * Builds on the bitmap PMM to provide O(log n) allocations for
 * contiguous frames (≥ 4 pages). For small (1-3 page) allocations,
 * the bitmap first-fit is used directly. For larger allocations,
 * the buddy layer maintains free lists per order (0..MAX_ORDER-1).
 *
 * Order k: 2^k contiguous pages. MAX_ORDER=11 → up to 8 MiB (2048 pages).
 *
 * Must be called after pmm_init() and before kheap_init(). */

#define PMM_BUDDY_MAX_ORDER 11
#define PMM_BUDDY_MAX_PAGES (1U << PMM_BUDDY_MAX_ORDER)  /* 2048 pages */

void pmm_buddy_init(void *serial_dev);
uint64_t pmm_buddy_alloc_pages(uint32_t count);
void pmm_buddy_free_pages(uint64_t addr, uint32_t count);
int pmm_buddy_verify(void);

#endif
