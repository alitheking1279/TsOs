#include "slab.h"
#include "kheap.h"
#include "pmm.h"
#include "spinlock.h"
#include "../drivers/serial.h"
#include <string.h>

/* Slab allocator for small objects.
 *
 * Size classes: 16, 32, 64, 128, 256, 512, 1024 bytes.
 * Each size class maintains a list of slab pages. Each slab page is
 * one 4 KiB frame subdivided into slots of the class size.
 *
 * Free slots store a pointer to the next free slot in the first 8 bytes.
 * When a slab page runs out of free slots, it is removed from the
 * free-list. When all slots are returned, the page is freed back to
 * the PMM via kfree(). */

#define SLAB_CLASS_COUNT 7

static const uint64_t slab_sizes[SLAB_CLASS_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024
};

/* Number of usable bytes per 4 KiB slab page, minus the header.
 * The header occupies the first portion of each slab page:
 *   slab_page_t struct (class_size, free_head, used_count, slot_count, next)
 *
 * IMPORTANT: SLAB_HEADER_SIZE must be at least sizeof(slab_page_t) and
 * aligned to 16 bytes.  Previously this was hardcoded to 16, but
 * sizeof(slab_page_t) is 40 bytes, causing metadata to overlap with
 * slot data — a critical corruption bug. */
#define SLAB_HEADER_SIZE ((sizeof(slab_page_t) + 15) & ~(uint64_t)15)

typedef struct slab_page {
    uint64_t class_size;
    uint64_t free_head;     /* index of first free slot, or count if full */
    uint64_t used_count;
    uint64_t slot_count;    /* total slots that fit */
    struct slab_page *next; /* next slab page in this class list */
} slab_page_t;

static slab_page_t *g_slab_lists[SLAB_CLASS_COUNT];
static int g_slab_initialized;
static serial_dev_t *g_slab_serial;
static spinlock_t g_slab_lock;
static uint64_t g_slab_allocated_bytes;

static void slab_log(const char *s)
{
    if (g_slab_serial) serial_write_string(g_slab_serial, s);
}

static void slab_log_u64(uint64_t v)
{
    if (!g_slab_serial) return;
    if (v == 0) { serial_write_char(g_slab_serial, '0'); return; }
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    while (v > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    serial_write_string(g_slab_serial, &buf[i]);
}

static int size_to_class(uint64_t size)
{
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        if (size <= slab_sizes[i]) return i;
    }
    return -1;
}

/* Free-slot link: each free slot's first 8 bytes point to the next free slot.
 * We store the index (0-based) into the slot array, NOT a pointer. */
static inline void *slot_addr(slab_page_t *sp, uint64_t slot_idx)
{
    uint64_t base = (uint64_t)sp;
    return (void *)(base + SLAB_HEADER_SIZE + slot_idx * sp->class_size);
}

static void slab_page_init(slab_page_t *sp, int class_idx)
{
    uint64_t obj_size = slab_sizes[class_idx];
    uint64_t usable = PAGE_SIZE - SLAB_HEADER_SIZE;
    uint64_t count = usable / obj_size;

    sp->class_size = obj_size;
    sp->slot_count = count;
    sp->used_count = 0;
    sp->next = NULL;

    /* Build free list: slot[i] -> slot[i+1] -> ... -> NULL(count) */
    for (uint64_t i = 0; i < count - 1; i++) {
        uint64_t *link = (uint64_t *)slot_addr(sp, i);
        *link = i + 1;
    }
    uint64_t *last = (uint64_t *)slot_addr(sp, count - 1);
    *last = count; /* sentinel: no next free */
    sp->free_head = 0;
}

static void *slab_page_alloc(slab_page_t *sp)
{
    if (sp->free_head >= sp->slot_count) return NULL;

    uint64_t slot = sp->free_head;
    uint64_t *link = (uint64_t *)slot_addr(sp, slot);
    sp->free_head = *link;
    sp->used_count++;

    return slot_addr(sp, slot);
}

static int slab_page_free(slab_page_t *sp, void *ptr)
{
    uint64_t base = (uint64_t)sp + SLAB_HEADER_SIZE;
    uint64_t addr = (uint64_t)ptr;

    if (addr < base) return 0;
    if (addr >= (uint64_t)sp + PAGE_SIZE) return 0;

    uint64_t offset = addr - base;
    if (offset % sp->class_size != 0) return 0;

    uint64_t slot = offset / sp->class_size;

    /* Double-free detection: walk the free list to see if slot is
     * already free. If it is, refuse to free again. */
    uint64_t check = sp->free_head;
    while (check < sp->slot_count) {
        if (check == slot) {
            slab_log("[SLAB] DOUBLE-FREE detected in slot ");
            slab_log_u64(slot);
            slab_log("\r\n");
            return 0;
        }
        uint64_t *link = (uint64_t *)slot_addr(sp, check);
        check = *link;
    }

    /* Link into free list */
    uint64_t *link = (uint64_t *)slot_addr(sp, slot);
    *link = sp->free_head;
    sp->free_head = slot;
    sp->used_count--;

    return 1;
}

static int slab_page_empty(slab_page_t *sp)
{
    return sp->used_count == 0;
}

void slab_init(void *serial_dev)
{
    g_slab_serial = (serial_dev_t *)serial_dev;
    spinlock_init(&g_slab_lock);

    for (int i = 0; i < SLAB_CLASS_COUNT; i++)
        g_slab_lists[i] = NULL;

    g_slab_allocated_bytes = 0;
    g_slab_initialized = 1;

    slab_log("[SLAB] Init complete. Classes: ");
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        slab_log_u64(slab_sizes[i]);
        if (i < SLAB_CLASS_COUNT - 1) slab_log(", ");
    }
    slab_log("\r\n");
}

void *slab_alloc(uint64_t size)
{
    if (!g_slab_initialized || size == 0) return NULL;

    int class_idx = size_to_class(size);

    /* Fall back to kmalloc for allocations larger than the biggest slab class. */
    if (class_idx < 0) {
        void *ptr = kmalloc(size);
        if (ptr) g_slab_allocated_bytes += size;
        return ptr;
    }

    uint64_t slab_rflags = spin_lock(&g_slab_lock);

    slab_page_t *sp = g_slab_lists[class_idx];
    while (sp && sp->free_head >= sp->slot_count)
        sp = sp->next;

    if (!sp) {
        /* Allocate a new slab page from the PMM.
         * kmalloc returns a full page which we overlay with slab metadata. */
        void *page = kmalloc(PAGE_SIZE);
        if (!page) {
            spin_unlock(&g_slab_lock, slab_rflags);
            slab_log("[SLAB] OOM: cannot allocate new page for class ");
            slab_log_u64(slab_sizes[class_idx]);
            slab_log("\r\n");
            return NULL;
        }

        sp = (slab_page_t *)page;
        slab_page_init(sp, class_idx);
        sp->next = g_slab_lists[class_idx];
        g_slab_lists[class_idx] = sp;
    }

    void *obj = slab_page_alloc(sp);
    if (obj) g_slab_allocated_bytes += sp->class_size;

    spin_unlock(&g_slab_lock, slab_rflags);
    return obj;
}

void slab_free(void *ptr)
{
    if (!g_slab_initialized || !ptr) return;

    uint64_t addr = (uint64_t)ptr;

    uint64_t slab_rflags = spin_lock(&g_slab_lock);

    /* Find which class this pointer belongs to by checking page alignment.
     * Each slab page is exactly PAGE_SIZE, so the slab_page_t header is
     * at the page-aligned base address. */
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        slab_page_t *sp = g_slab_lists[i];
        while (sp) {
            uint64_t page_base = (uint64_t)sp;
            if (addr >= page_base && addr < page_base + PAGE_SIZE) {
                if (slab_page_free(sp, ptr)) {
                    g_slab_allocated_bytes -= sp->class_size;

                    /* If the page is now empty, free it back to the heap. */
                    if (slab_page_empty(sp)) {
                        /* Remove from list */
                        slab_page_t **pp = &g_slab_lists[i];
                        while (*pp && *pp != sp)
                            pp = &(*pp)->next;
                        if (*pp) *pp = sp->next;

                        spin_unlock(&g_slab_lock, slab_rflags);
                        kfree(sp);
                        return;
                    }
                    spin_unlock(&g_slab_lock, slab_rflags);
                    return;
                }
            }
            sp = sp->next;
        }
    }

    spin_unlock(&g_slab_lock, slab_rflags);

    /* Pointer not in any slab — it was allocated via kmalloc fallback.
     * Free it back to the heap. We don't know the exact size here,
     * so the allocated bytes counter may be slightly inaccurate. */
    kfree(ptr);
    return;
}

uint64_t slab_get_allocated_bytes(void)
{
    return g_slab_allocated_bytes;
}
