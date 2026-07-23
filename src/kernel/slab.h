#ifndef KERNEL_SLAB_H
#define KERNEL_SLAB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slab allocator — small-object cache for allocations ≤ 1024 bytes.
 *
 * Size classes: 16, 32, 64, 128, 256, 512, 1024 bytes.
 * Objects are drawn from 4 KiB pages (one slab per page).
 * Free objects are linked via an intrusive free-list pointer in the
 * first 8 bytes of each free slot.
 *
 * slab_init() must be called after kheap_init().
 * slab_alloc()/slab_free() route through the slab for small sizes
 * and fall back to kmalloc/kfree for larger allocations. */

typedef enum {
    SLAB_OK           =  0,
    SLAB_ERR_NOT_INIT = -1,
    SLAB_ERR_NO_MEM   = -2,
} slab_status_t;

void slab_init(void *serial_dev);
void *slab_alloc(uint64_t size);
void  slab_free(void *ptr);
uint64_t slab_get_allocated_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_SLAB_H */
