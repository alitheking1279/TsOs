#include "mem_stats.h"
#include "pmm.h"
#include "kheap.h"
#include "slab.h"
#include "../lib/print.h"

/* Unified memory statistics — dumps PMM, kheap, slab stats in one call.
 * Uses print.h utilities to avoid duplicating hex/decimal formatters. */

static serial_dev_t *g_stats_serial;

static void ms_str(const char *s) { print_str(g_stats_serial, s); }
static void ms_hex(uint64_t v)    { print_hex64(g_stats_serial, v); }
static void ms_u64(uint64_t v)    { print_uint64(g_stats_serial, v); }

void mem_stats_init(void *serial_dev)
{
    g_stats_serial = (serial_dev_t *)serial_dev;
}

void mem_stats_dump(void)
{
    ms_str("\r\n");
    ms_str("============================================\r\n");
    ms_str("         Memory Statistics Dump             \r\n");
    ms_str("============================================\r\n");

    /* PMM stats */
    pmm_stats_t pmm;
    pmm_get_stats(&pmm);
    ms_str("[PMM] Total frames    : "); ms_u64(pmm.total_frames);
    ms_str(" ("); ms_u64(pmm.total_frames * 4); ms_str(" KiB)\r\n");
    ms_str("[PMM] Free frames     : "); ms_u64(pmm.free_frames);
    ms_str(" ("); ms_u64(pmm.free_frames * 4); ms_str(" KiB)\r\n");
    ms_str("[PMM] Used frames     : "); ms_u64(pmm.used_frames);
    ms_str(" ("); ms_u64(pmm.used_frames * 4); ms_str(" KiB)\r\n");
    ms_str("[PMM] Reserved frames : "); ms_u64(pmm.reserved_frames);
    ms_str(" ("); ms_u64(pmm.reserved_frames * 4); ms_str(" KiB)\r\n");

    /* kheap stats */
    kheap_stats_t kh;
    kheap_get_stats(&kh);
    ms_str("[KHEAP] Total pages     : "); ms_u64(kh.total_pages);
    ms_str(" ("); ms_u64(kh.total_pages * 4); ms_str(" KiB)\r\n");
    ms_str("[KHEAP] Allocated pages : "); ms_u64(kh.allocated_pages);
    ms_str(" ("); ms_u64(kh.allocated_pages * 4); ms_str(" KiB)\r\n");
    ms_str("[KHEAP] Free pages      : "); ms_u64(kh.free_pages);
    ms_str(" ("); ms_u64(kh.free_pages * 4); ms_str(" KiB)\r\n");
    ms_str("[KHEAP] Active allocs   : "); ms_u64(kh.active_allocs); ms_str("\r\n");
    ms_str("[KHEAP] Total allocs    : "); ms_u64(kh.total_allocs); ms_str("\r\n");

    /* Slab stats */
    ms_str("[SLAB] Allocated bytes  : "); ms_u64(slab_get_allocated_bytes()); ms_str("\r\n");

    ms_str("============================================\r\n");
}
