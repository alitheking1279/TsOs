/**
 * @file malloc.c
 * @brief Userspace heap allocator — brk-based, first-fit free list.
 *
 * Uses SYS_BRK to grow/shrink the heap. Maintains a singly-linked free
 * list with coalescing on free. Minimum allocation 16 bytes.
 *
 * This is the key allocator for userspace programs (including doomgeneric).
 */

#include <stddef.h>
#include <stdint.h>

/* Provided by syscalls.c */
extern long sys_brk(uint64_t addr);

/* =========================================================================
 * Constants
 * ========================================================================= */

#define PAGE_SIZE       4096
#define MIN_BLOCK_SIZE  16
#define BLOCK_HDR_SIZE  sizeof(block_header_t)

/* =========================================================================
 * Block Header — prepended to every allocation
 * ========================================================================= */

typedef struct block_header {
    size_t              size;       /* Usable size (excludes header). */
    int                 free;       /* 1 = free, 0 = allocated. */
    struct block_header *next;      /* Next block in address order. */
} block_header_t;

/* =========================================================================
 * State
 * ========================================================================= */

/** Head of the block list (address-ordered). */
static block_header_t *g_heap_head = (void *)0;

/** Current brk (end of heap). */
static uint64_t g_heap_brk = 0;

/** Base of the heap (first brk). */
static uint64_t g_heap_base = 0;

/* =========================================================================
 * Helpers
 * ========================================================================= */

/** Round up to next multiple of PAGE_SIZE. */
static size_t page_align(size_t size) {
    return (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
}

/** Extend heap by `nbytes` (page-aligned). Returns 0 on success, -1 on failure. */
static long grow_heap(size_t nbytes) {
    size_t pages = page_align(nbytes);
    uint64_t new_brk = g_heap_brk + pages;
    long ret = sys_brk(new_brk);
    if (ret < 0) return -1;
    g_heap_brk = (uint64_t)ret;
    return 0;
}

/* =========================================================================
 * Split — carve a block if it's significantly larger than needed
 * ========================================================================= */

static void split_block(block_header_t *blk, size_t size) {
    if (blk->size >= size + BLOCK_HDR_SIZE + MIN_BLOCK_SIZE) {
        block_header_t *new_blk =
            (block_header_t *)((uint8_t *)blk + BLOCK_HDR_SIZE + size);
        new_blk->size = blk->size - size - BLOCK_HDR_SIZE;
        new_blk->free = 1;
        new_blk->next = blk->next;
        blk->next = new_blk;
        blk->size = size;
    }
}

/* =========================================================================
 * Coalesce — merge adjacent free blocks
 * ========================================================================= */

static void coalesce(void) {
    block_header_t *cur = g_heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += BLOCK_HDR_SIZE + cur->next->size;
            cur->next = cur->next->next;
            /* Don't advance — check again in case next-next is also free. */
        } else {
            cur = cur->next;
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void *malloc(size_t size) {
    if (size == 0) return (void *)0;

    /* Align size to 8-byte boundary for alignment. */
    size = (size + 7) & ~(size_t)7;
    if (size < MIN_BLOCK_SIZE) size = MIN_BLOCK_SIZE;

    /* First call: initialize the heap. */
    if (g_heap_base == 0) {
        long ret = sys_brk(0);
        if (ret < 0) return (void *)0;
        g_heap_base = (uint64_t)ret;
        g_heap_brk = g_heap_base;
    }

    /* First-fit search. */
    block_header_t *cur = g_heap_head;
    while (cur) {
        if (cur->free && cur->size >= size) {
            split_block(cur, size);
            cur->free = 0;
            return (void *)((uint8_t *)cur + BLOCK_HDR_SIZE);
        }
        cur = cur->next;
    }

    /* No free block found — grow the heap. */
    size_t needed = size + BLOCK_HDR_SIZE;
    if (grow_heap(needed) != 0) return (void *)0;

    /* The new block is at the old brk. */
    block_header_t *new_blk = (block_header_t *)g_heap_brk
                              - (needed / BLOCK_HDR_SIZE + 1);
    /* Simpler: place it at the end of the last block or at base. */
    if (g_heap_head) {
        /* Find the last block. */
        block_header_t *last = g_heap_head;
        while (last->next) last = last->next;
        /* Check if last block is free and adjacent — extend it. */
        if (last->free) {
            last->size += needed;
            split_block(last, size);
            last->free = 0;
            return (void *)((uint8_t *)last + BLOCK_HDR_SIZE);
        }
        new_blk = (block_header_t *)((uint8_t *)last + BLOCK_HDR_SIZE
                                     + last->size);
    } else {
        new_blk = (block_header_t *)g_heap_base;
    }

    new_blk->size = needed - BLOCK_HDR_SIZE;
    new_blk->free = 0;
    new_blk->next = (void *)0;

    if (g_heap_head) {
        block_header_t *last = g_heap_head;
        while (last->next) last = last->next;
        last->next = new_blk;
    } else {
        g_heap_head = new_blk;
    }

    return (void *)((uint8_t *)new_blk + BLOCK_HDR_SIZE);
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return (void *)0;
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) return (void *)0;

    void *p = malloc(total);
    if (!p) return (void *)0;

    /* Zero-fill. */
    uint8_t *dst = (uint8_t *)p;
    for (size_t i = 0; i < total; i++) dst[i] = 0;
    return p;
}

void free(void *ptr) {
    if (!ptr) return;

    block_header_t *blk = (block_header_t *)((uint8_t *)ptr - BLOCK_HDR_SIZE);
    blk->free = 1;
    coalesce();
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return (void *)0; }

    block_header_t *blk = (block_header_t *)((uint8_t *)ptr - BLOCK_HDR_SIZE);
    if (blk->size >= size) {
        /* Current block is large enough. */
        return ptr;
    }

    /* Try to extend in-place if next block is free. */
    if (blk->next && blk->next->free) {
        size_t avail = blk->size + BLOCK_HDR_SIZE + blk->next->size;
        if (avail >= size) {
            blk->size += BLOCK_HDR_SIZE + blk->next->size;
            blk->next = blk->next->next;
            split_block(blk, size);
            return ptr;
        }
    }

    /* Allocate new block and copy. */
    void *new_ptr = malloc(size);
    if (!new_ptr) return (void *)0;

    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)new_ptr;
    size_t copy_size = blk->size < size ? blk->size : size;
    for (size_t i = 0; i < copy_size; i++) dst[i] = src[i];

    free(ptr);
    return new_ptr;
}
