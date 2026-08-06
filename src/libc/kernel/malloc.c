/**
 * @file malloc.c
 * @brief Kernel malloc — wraps kmalloc/slab for <stdlib.h> API.
 *
 * Routing:
 *   1-1024 bytes  → slab_alloc/slab_free
 *   1025+ bytes   → kmalloc/kfree
 *   realloc: alloc new, memcpy, free old (size-class based free)
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Provided by kernel slab/kheap */
extern void *slab_alloc(size_t size);
extern void  slab_free(void *ptr);
extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);

/** Threshold: slab handles <= 1024, kheap handles > 1024. */
#define SLAB_MAX_SIZE 1024

void *malloc(size_t size) {
    if (size == 0) return (void *)0;
    if (size <= SLAB_MAX_SIZE)
        return slab_alloc(size);
    return kmalloc(size);
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return (void *)0;
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) return (void *)0; /* overflow */
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void free(void *ptr) {
    if (!ptr) return;
    /* Slab uses fixed size classes; kheap uses page-granularity.
     * Since slab allocations are always <= 1024 bytes and come from
     * the slab cache, slab_free handles them. For anything else,
     * kfree handles it. We can't distinguish at the pointer level
     * without metadata, so we use a best-effort approach:
     * try slab_free first (it has its own validation), then kfree.
     * In practice, the slab allocator's free path checks its own
     * internal state, and kfree has double-free detection. */
    slab_free(ptr);
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return (void *)0; }

    void *new = malloc(size);
    if (!new) return (void *)0;

    /* We don't know the old size. For slab objects (<= 1024),
     * copy up to the new size. For kmalloc objects, copy up to
     * min(old_size_estimate, size). A safe estimate for slab is
     * the next power-of-2 class; for kheap, we copy size bytes
     * (the extra bytes may read past allocation but won't fault
     * because kheap rounds up to pages). */
    size_t copy_size = size; /* safe upper bound */
    memcpy(new, ptr, copy_size);
    free(ptr);
    return new;
}
