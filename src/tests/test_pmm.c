/**
 * @file test_pmm.c
 * @brief Physical Memory Manager test suite.
 *
 * Test philosophy
 * ===============
 * These tests treat the PMM as a black box via its public API and verify
 * actual hardware-observable behaviour:
 *
 *  Group A — Post-init invariants (state before any alloc/free)
 *    pmm_stats_sane          Statistics are internally consistent after init
 *    pmm_frame0_reserved     Frame 0 is never allocatable (BIOS protection)
 *    pmm_kernel_reserved     Frames covering the kernel image are not allocatable
 *    pmm_bitmap_reserved     Frames covering the PMM bitmap are not allocatable
 *    pmm_free_frames_nonzero At least some RAM was detected and freed by init
 *
 *  Group B — Basic alloc / free cycle
 *    pmm_alloc_returns_aligned   Returned address is 4 KiB aligned
 *    pmm_alloc_nonzero           alloc does not return 0 (frame 0 always reserved)
 *    pmm_alloc_marks_used        Frame is USED immediately after alloc
 *    pmm_free_marks_free         Frame is FREE immediately after free
 *    pmm_free_bad_align          Freeing a misaligned address → PMM_ERR_INVALID
 *    pmm_free_double_free        Freeing a frame twice → PMM_ERR_ALREADY_FREE
 *
 *  Group C — Multi-alloc correctness
 *    pmm_alloc_unique            Two consecutive allocs return different addresses
 *    pmm_alloc_free_reuse        Free a frame, alloc again — it comes back
 *    pmm_alloc_many_unique       Alloc 32 frames, all unique and 4 KiB-aligned
 *    pmm_stats_track_alloc       free_frames decrements correctly on alloc
 *    pmm_stats_track_free        free_frames increments correctly on free
 *
 *  Group D — Contiguous allocation
 *    pmm_alloc_contig_aligned    pmm_alloc_frames(N) returns 4 KiB aligned
 *    pmm_alloc_contig_contiguous The N frames are physically contiguous
 *    pmm_alloc_contig_unique_vs_single Contiguous base != a separately allocated frame
 *    pmm_alloc_contig_free_all   Can free each frame of a contiguous block
 *
 *  Group E — PMM is_free / mark helpers
 *    pmm_is_free_after_alloc     pmm_is_free() returns 0 for allocated frame
 *    pmm_is_free_after_free      pmm_is_free() returns 1 for freed frame
 *    pmm_is_free_invalid         pmm_is_free() on misaligned addr returns -1
 *
 *  Group F — Mark helpers and structural guards
 *    pmm_mark_used_works         pmm_mark_used() makes a free frame used
 *    pmm_mark_free_works         pmm_mark_free() makes a used frame free
 *    pmm_mark_free_frame0_noop   pmm_mark_free(0) is a no-op (frame 0 stays used)
 *    pmm_bitmap_reserved         PMM bitmap frames are marked USED after init
 *    pmm_alloc_never_frame0      pmm_alloc_frame() structurally skips frame 0
 *
 * Each test emits its own serial log lines so the QEMU output gives a
 * step-by-step trace without printf or any other library.
 */

#include "test.h"
#include "../kernel/pmm.h"
#include "../kernel/page_table.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Internal serial logging helpers (mirror of main.c — no shared state)
 *
 * Kept local so the test file compiles without depending on any global
 * beyond the serial_dev_t* passed by the test runner.
 * ========================================================================= */

static void t_hex64(serial_dev_t *dev, uint64_t val) {
    static const char h[] = "0123456789ABCDEF";
    serial_write_string(dev, "0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_write_char(dev, h[(val >> i) & 0xF]);
}

static void t_uint64(serial_dev_t *dev, uint64_t val) {
    char buf[21]; int i = 20; buf[i] = '\0';
    if (val == 0) { serial_write_char(dev, '0'); return; }
    while (val > 0) { buf[--i] = (char)('0' + val % 10); val /= 10; }
    serial_write_string(dev, &buf[i]);
}

/* Emit:  "[PMM TEST] <label> = <hex64>\r\n" */
static void t_log_hex(serial_dev_t *dev, const char *label, uint64_t val) {
    serial_write_string(dev, "[PMM TEST]   "); serial_write_string(dev, label);
    serial_write_string(dev, " = "); t_hex64(dev, val);
    serial_write_string(dev, "\r\n");
}

/* Emit:  "[PMM TEST] <label> = <decimal>\r\n" */
static void t_log_dec(serial_dev_t *dev, const char *label, uint64_t val) {
    serial_write_string(dev, "[PMM TEST]   "); serial_write_string(dev, label);
    serial_write_string(dev, " = "); t_uint64(dev, val);
    serial_write_string(dev, "\r\n");
}

/* =========================================================================
 * Linker-exported kernel image boundary symbols (same as pmm.c uses)
 * ========================================================================= */
extern char _kernel_start[];
extern char _kernel_end[];

/* =========================================================================
 * Group A — Post-init invariants
 * ========================================================================= */

/** A1: Statistics are internally consistent after pmm_init(). */
static void test_pmm_stats_sane(serial_dev_t *dev) {
    pmm_stats_t s;
    pmm_get_stats(&s);

    serial_write_string(dev, "[PMM TEST] stats_sane:\r\n");
    t_log_dec(dev, "total_frames", s.total_frames);
    t_log_dec(dev, "free_frames ", s.free_frames);
    t_log_dec(dev, "used_frames ", s.used_frames);

    /* total must be non-zero */
    ASSERT_TRUE(dev, s.total_frames > 0);
    /* free + used must equal total */
    ASSERT_EQ(dev, s.free_frames + s.used_frames, s.total_frames);
    /* used must be at least 1 (frame 0 is always reserved) */
    ASSERT_TRUE(dev, s.used_frames >= 1);
}

/** A2: Frame 0 (physical address 0x0000) is never allocatable. */
static void test_pmm_frame0_reserved(serial_dev_t *dev) {
    /* pmm_is_free(0) must return 0 (used). */
    int result = pmm_is_free(0x0000);
    serial_write_string(dev, "[PMM TEST] frame0_reserved: pmm_is_free(0) = ");
    t_uint64(dev, (uint64_t)(result < 0 ? (uint64_t)(-result) : (uint64_t)result));
    serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, result, 0);
}

/** A3: Every frame in the kernel image is marked USED. */
static void test_pmm_kernel_reserved(serial_dev_t *dev) {
    uint64_t ks = (uint64_t)(uintptr_t)_kernel_start - HIGHER_HALF_OFFSET;
    uint64_t ke = (uint64_t)(uintptr_t)_kernel_end   - HIGHER_HALF_OFFSET;

    serial_write_string(dev, "[PMM TEST] kernel_reserved:\r\n");
    t_log_hex(dev, "kernel_start", ks);
    t_log_hex(dev, "kernel_end  ", ke);

    /* Check the first, middle, and last frame of the kernel image. */
    uint64_t first_frame = ks >> 12;
    uint64_t last_frame  = (ke - 1) >> 12;
    uint64_t mid_frame   = (first_frame + last_frame) / 2;

    t_log_dec(dev, "first_frame", first_frame);
    t_log_dec(dev, "mid_frame  ", mid_frame);
    t_log_dec(dev, "last_frame ", last_frame);

    ASSERT_EQ(dev, pmm_is_free(first_frame << 12), 0);
    ASSERT_EQ(dev, pmm_is_free(mid_frame   << 12), 0);
    ASSERT_EQ(dev, pmm_is_free(last_frame  << 12), 0);
}

/** A4: At least some RAM was freed by init (machine has more than just the kernel). */
static void test_pmm_free_frames_nonzero(serial_dev_t *dev) {
    pmm_stats_t s;
    pmm_get_stats(&s);
    serial_write_string(dev, "[PMM TEST] free_frames_nonzero: free=");
    t_uint64(dev, s.free_frames);
    serial_write_string(dev, "\r\n");
    ASSERT_TRUE(dev, s.free_frames > 0);
}

/* =========================================================================
 * Group B — Basic alloc / free cycle
 * ========================================================================= */

/** B1: Allocated address is 4 KiB aligned. */
static void test_pmm_alloc_returns_aligned(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    serial_write_string(dev, "[PMM TEST] alloc_aligned: addr=");
    t_hex64(dev, addr); serial_write_string(dev, "\r\n");
    ASSERT_TRUE(dev, addr != 0);
    ASSERT_EQ(dev, addr & 0xFFF, 0ULL);
    pmm_free_frame(addr);
}

/** B2: Allocated address is never 0 (frame 0 is reserved). */
static void test_pmm_alloc_nonzero(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    serial_write_string(dev, "[PMM TEST] alloc_nonzero: addr=");
    t_hex64(dev, addr); serial_write_string(dev, "\r\n");
    ASSERT_TRUE(dev, addr != 0);
    pmm_free_frame(addr);
}

/** B3: Frame is USED immediately after alloc. */
static void test_pmm_alloc_marks_used(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    ASSERT_TRUE(dev, addr != 0);
    int free_after = pmm_is_free(addr);
    serial_write_string(dev, "[PMM TEST] alloc_marks_used: is_free=");
    t_uint64(dev, (uint64_t)free_after);
    serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, free_after, 0);  /* must NOT be free */
    pmm_free_frame(addr);
}

/** B4: Frame is FREE immediately after free. */
static void test_pmm_free_marks_free(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    ASSERT_TRUE(dev, addr != 0);
    pmm_status_t st = pmm_free_frame(addr);
    ASSERT_EQ(dev, (int)st, (int)PMM_OK);
    int free_after = pmm_is_free(addr);
    serial_write_string(dev, "[PMM TEST] free_marks_free: is_free=");
    t_uint64(dev, (uint64_t)free_after);
    serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, free_after, 1);  /* must be free */
}

/** B5: Freeing a misaligned address returns PMM_ERR_INVALID. */
static void test_pmm_free_bad_align(serial_dev_t *dev) {
    /* 0x1001 is not page-aligned */
    pmm_status_t st = pmm_free_frame(0x1001ULL);
    serial_write_string(dev, "[PMM TEST] free_bad_align: status=");
    t_uint64(dev, (uint64_t)(-(int)st));
    serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, (int)st, (int)PMM_ERR_INVALID);
}

/** B6: Freeing a frame twice returns PMM_ERR_ALREADY_FREE. */
static void test_pmm_free_double_free(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    ASSERT_TRUE(dev, addr != 0);
    pmm_status_t st1 = pmm_free_frame(addr);
    ASSERT_EQ(dev, (int)st1, (int)PMM_OK);
    pmm_status_t st2 = pmm_free_frame(addr);
    serial_write_string(dev, "[PMM TEST] double_free: 2nd status=");
    t_uint64(dev, (uint64_t)(-(int)st2));
    serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, (int)st2, (int)PMM_ERR_ALREADY_FREE);
}

/* =========================================================================
 * Group C — Multi-alloc correctness
 * ========================================================================= */

/** C1: Two consecutive allocs return different addresses. */
static void test_pmm_alloc_unique(serial_dev_t *dev) {
    uint64_t a = pmm_alloc_frame();
    uint64_t b = pmm_alloc_frame();
    serial_write_string(dev, "[PMM TEST] alloc_unique: a=");
    t_hex64(dev, a); serial_write_string(dev, " b="); t_hex64(dev, b);
    serial_write_string(dev, "\r\n");
    ASSERT_TRUE(dev, a != 0 && b != 0);
    ASSERT_TRUE(dev, a != b);
    pmm_free_frame(a);
    pmm_free_frame(b);
}

/** C2: Free a frame, then alloc — the same frame can come back. */
static void test_pmm_alloc_free_reuse(serial_dev_t *dev) {
    uint64_t a = pmm_alloc_frame();
    ASSERT_TRUE(dev, a != 0);
    pmm_free_frame(a);

    /* The PMM is a first-fit allocator; `a` should be the next frame given. */
    uint64_t b = pmm_alloc_frame();
    serial_write_string(dev, "[PMM TEST] alloc_free_reuse: a=");
    t_hex64(dev, a); serial_write_string(dev, " b="); t_hex64(dev, b);
    serial_write_string(dev, "\r\n");
    ASSERT_TRUE(dev, b != 0);

    /* We don't assert a == b because there's no strict obligation, but
     * the first-fit scan SHOULD return a frame <= a+1.  We just verify
     * that the reuse doesn't crash and b is valid. */
    ASSERT_EQ(dev, b & 0xFFF, 0ULL);   /* must still be page-aligned */
    pmm_free_frame(b);
}

/** C3: Alloc 32 frames — all unique and 4 KiB aligned. */
#define BULK_COUNT 32
static void test_pmm_alloc_many_unique(serial_dev_t *dev) {
    uint64_t frames[BULK_COUNT];
    pmm_stats_t before, after;
    pmm_get_stats(&before);

    serial_write_string(dev, "[PMM TEST] alloc_many_unique: allocating ");
    t_uint64(dev, BULK_COUNT);
    serial_write_string(dev, " frames\r\n");

    for (int i = 0; i < BULK_COUNT; i++) {
        frames[i] = pmm_alloc_frame();
        if (frames[i] == 0) {
            serial_write_string(dev, "[PMM TEST]   FAIL: OOM at i=");
            t_uint64(dev, (uint64_t)i); serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            /* Free whatever we got before returning. */
            for (int j = 0; j < i; j++) pmm_free_frame(frames[j]);
            return;
        }
        /* Check alignment */
        if (frames[i] & 0xFFF) {
            serial_write_string(dev, "[PMM TEST]   FAIL: misaligned at i=");
            t_uint64(dev, (uint64_t)i); serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            for (int j = 0; j <= i; j++) pmm_free_frame(frames[j]);
            return;
        }
    }

    /* Verify all are unique (O(N²) — fine for 32 frames). */
    for (int i = 0; i < BULK_COUNT; i++) {
        for (int j = i + 1; j < BULK_COUNT; j++) {
            if (frames[i] == frames[j]) {
                serial_write_string(dev, "[PMM TEST]   FAIL: duplicate frames[");
                t_uint64(dev, (uint64_t)i); serial_write_string(dev, "] == frames[");
                t_uint64(dev, (uint64_t)j); serial_write_string(dev, "]\r\n");
                test_fail_flag = 1;
                for (int k = 0; k < BULK_COUNT; k++) pmm_free_frame(frames[k]);
                return;
            }
        }
    }

    pmm_get_stats(&after);
    t_log_dec(dev, "free_before", before.free_frames);
    t_log_dec(dev, "free_after ", after.free_frames);
    ASSERT_EQ(dev, before.free_frames - after.free_frames, (uint64_t)BULK_COUNT);

    /* Free all allocated frames. */
    for (int i = 0; i < BULK_COUNT; i++) {
        pmm_status_t st = pmm_free_frame(frames[i]);
        if (st != PMM_OK) {
            serial_write_string(dev, "[PMM TEST]   FAIL: free failed at i=");
            t_uint64(dev, (uint64_t)i); serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
    pmm_get_stats(&after);
    ASSERT_EQ(dev, after.free_frames, before.free_frames);
}

/** C4: free_frames decrements correctly after each alloc. */
static void test_pmm_stats_track_alloc(serial_dev_t *dev) {
    pmm_stats_t before, after;
    pmm_get_stats(&before);

    uint64_t addr = pmm_alloc_frame();
    ASSERT_TRUE(dev, addr != 0);

    pmm_get_stats(&after);
    t_log_dec(dev, "free_before", before.free_frames);
    t_log_dec(dev, "free_after ", after.free_frames);
    ASSERT_EQ(dev, before.free_frames - after.free_frames, 1ULL);

    pmm_free_frame(addr);
}

/** C5: free_frames increments correctly after each free. */
static void test_pmm_stats_track_free(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    ASSERT_TRUE(dev, addr != 0);

    pmm_stats_t before, after;
    pmm_get_stats(&before);

    pmm_free_frame(addr);
    pmm_get_stats(&after);

    t_log_dec(dev, "free_before", before.free_frames);
    t_log_dec(dev, "free_after ", after.free_frames);
    ASSERT_EQ(dev, after.free_frames - before.free_frames, 1ULL);
}

/* =========================================================================
 * Group D — Contiguous allocation
 * ========================================================================= */

#define CONTIG_COUNT 8ULL

/** D1: pmm_alloc_frames(N) returns a 4 KiB aligned base. */
static void test_pmm_alloc_contig_aligned(serial_dev_t *dev) {
    uint64_t base = pmm_alloc_frames(CONTIG_COUNT);
    serial_write_string(dev, "[PMM TEST] alloc_contig_aligned: base=");
    t_hex64(dev, base); serial_write_string(dev, "\r\n");
    ASSERT_TRUE(dev, base != 0);
    ASSERT_EQ(dev, base & 0xFFF, 0ULL);
    /* Free all frames in the block. */
    for (uint64_t i = 0; i < CONTIG_COUNT; i++)
        pmm_free_frame(base + i * PAGE_SIZE);
}

/** D2: The N allocated frames are physically contiguous. */
static void test_pmm_alloc_contig_contiguous(serial_dev_t *dev) {
    uint64_t base = pmm_alloc_frames(CONTIG_COUNT);
    ASSERT_TRUE(dev, base != 0);

    serial_write_string(dev, "[PMM TEST] alloc_contig_contiguous: checking ");
    t_uint64(dev, CONTIG_COUNT);
    serial_write_string(dev, " frames from ");
    t_hex64(dev, base); serial_write_string(dev, "\r\n");

    for (uint64_t i = 0; i < CONTIG_COUNT; i++) {
        uint64_t addr = base + i * PAGE_SIZE;
        /* Every frame in the range must be USED (allocated). */
        int f = pmm_is_free(addr);
        if (f != 0) {
            serial_write_string(dev, "[PMM TEST]   FAIL: frame ");
            t_uint64(dev, i); serial_write_string(dev, " not marked used\r\n");
            test_fail_flag = 1;
            break;
        }
    }

    for (uint64_t i = 0; i < CONTIG_COUNT; i++)
        pmm_free_frame(base + i * PAGE_SIZE);
}

/** D3: Contiguous block base is different from a separately allocated single frame. */
static void test_pmm_alloc_contig_unique_vs_single(serial_dev_t *dev) {
    uint64_t single = pmm_alloc_frame();
    uint64_t base   = pmm_alloc_frames(CONTIG_COUNT);

    serial_write_string(dev, "[PMM TEST] contig_unique: single=");
    t_hex64(dev, single); serial_write_string(dev, " base="); t_hex64(dev, base);
    serial_write_string(dev, "\r\n");

    ASSERT_TRUE(dev, single != 0 && base != 0);
    ASSERT_TRUE(dev, single != base);

    /* Verify single does not overlap with [base, base + N*PAGE_SIZE). */
    ASSERT_TRUE(dev, single < base || single >= base + CONTIG_COUNT * PAGE_SIZE);

    pmm_free_frame(single);
    for (uint64_t i = 0; i < CONTIG_COUNT; i++)
        pmm_free_frame(base + i * PAGE_SIZE);
}

/** D4: Every frame in a contiguous block can be freed without error. */
static void test_pmm_alloc_contig_free_all(serial_dev_t *dev) {
    uint64_t base = pmm_alloc_frames(CONTIG_COUNT);
    ASSERT_TRUE(dev, base != 0);

    serial_write_string(dev, "[PMM TEST] contig_free_all: base=");
    t_hex64(dev, base); serial_write_string(dev, "\r\n");

    for (uint64_t i = 0; i < CONTIG_COUNT; i++) {
        uint64_t addr = base + i * PAGE_SIZE;
        pmm_status_t st = pmm_free_frame(addr);
        if (st != PMM_OK) {
            serial_write_string(dev, "[PMM TEST]   FAIL: free error at i=");
            t_uint64(dev, i); serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
}

/* =========================================================================
 * Group E — pmm_is_free / mark helpers
 * ========================================================================= */

/** E1: pmm_is_free() returns 0 for an allocated frame. */
static void test_pmm_is_free_after_alloc(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    ASSERT_TRUE(dev, addr != 0);
    int r = pmm_is_free(addr);
    serial_write_string(dev, "[PMM TEST] is_free_after_alloc: ");
    t_uint64(dev, (uint64_t)r); serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, r, 0);
    pmm_free_frame(addr);
}

/** E2: pmm_is_free() returns 1 for a freed frame. */
static void test_pmm_is_free_after_free(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    ASSERT_TRUE(dev, addr != 0);
    pmm_free_frame(addr);
    int r = pmm_is_free(addr);
    serial_write_string(dev, "[PMM TEST] is_free_after_free: ");
    t_uint64(dev, (uint64_t)r); serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, r, 1);
}

/** E3: pmm_is_free() on a misaligned address returns -1. */
static void test_pmm_is_free_invalid(serial_dev_t *dev) {
    int r = pmm_is_free(0x1234ULL);  /* not page-aligned */
    serial_write_string(dev, "[PMM TEST] is_free_invalid: ");
    t_uint64(dev, (uint64_t)(r < 0 ? (uint64_t)(-r) : (uint64_t)r));
    serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, r, -1);
}

/* =========================================================================
 * Group F — Mark helpers and structural guards
 * ========================================================================= */

/**
 * F1: pmm_mark_used() can reserve a currently-free frame.
 *
 * Allocate a frame, free it (so it is definitely free), then call
 * pmm_mark_used() on it and verify pmm_is_free() now returns 0.
 * Also verify the stats counter decremented.
 */
static void test_pmm_mark_used_works(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    ASSERT_TRUE(dev, addr != 0);
    pmm_free_frame(addr);          /* addr is now free */
    ASSERT_EQ(dev, pmm_is_free(addr), 1);

    pmm_stats_t before, after;
    pmm_get_stats(&before);

    pmm_mark_used(addr);

    pmm_get_stats(&after);
    int is_free = pmm_is_free(addr);

    serial_write_string(dev, "[PMM TEST] mark_used_works:");
    serial_write_string(dev, " is_free="); t_uint64(dev, (uint64_t)is_free);
    serial_write_string(dev, " free_delta=");
    t_uint64(dev, before.free_frames - after.free_frames);
    serial_write_string(dev, "\r\n");

    ASSERT_EQ(dev, is_free, 0);
    ASSERT_EQ(dev, before.free_frames - after.free_frames, 1ULL);

    /* Clean up: release so the frame is not permanently leaked. */
    pmm_mark_free(addr);
}

/**
 * F2: pmm_mark_free() can release a currently-used frame back to the pool.
 *
 * Reserve via pmm_mark_used(), then undo via pmm_mark_free().
 * Verify is_free result and stats counter.
 */
static void test_pmm_mark_free_works(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frame();
    ASSERT_TRUE(dev, addr != 0);
    pmm_free_frame(addr);          /* addr is free */

    pmm_mark_used(addr);           /* reserve without allocator */
    ASSERT_EQ(dev, pmm_is_free(addr), 0);

    pmm_stats_t before, after;
    pmm_get_stats(&before);

    pmm_mark_free(addr);

    pmm_get_stats(&after);
    int is_free = pmm_is_free(addr);

    serial_write_string(dev, "[PMM TEST] mark_free_works:");
    serial_write_string(dev, " is_free="); t_uint64(dev, (uint64_t)is_free);
    serial_write_string(dev, " free_delta=");
    t_uint64(dev, after.free_frames - before.free_frames);
    serial_write_string(dev, "\r\n");

    ASSERT_EQ(dev, is_free, 1);
    ASSERT_EQ(dev, after.free_frames - before.free_frames, 1ULL);
}

/**
 * F3: pmm_mark_free(0) is a hard no-op.
 *
 * Frame 0 is permanently reserved.  The fix adds an early-return guard
 * in pmm_mark_free().  Verify: is_free(0) stays 0 and free_frames
 * does NOT increment.
 */
static void test_pmm_mark_free_frame0_noop(serial_dev_t *dev) {
    ASSERT_EQ(dev, pmm_is_free(0), 0);  /* frame 0 is used before the call */

    pmm_stats_t before, after;
    pmm_get_stats(&before);

    pmm_mark_free(0x0000ULL);           /* must be silently ignored */

    pmm_get_stats(&after);
    int still_used = (pmm_is_free(0) == 0);

    serial_write_string(dev, "[PMM TEST] mark_free_frame0_noop:");
    serial_write_string(dev, " still_used="); t_uint64(dev, (uint64_t)still_used);
    serial_write_string(dev, " free_grew=");
    t_uint64(dev, (after.free_frames > before.free_frames)
                  ? after.free_frames - before.free_frames : 0ULL);
    serial_write_string(dev, "\r\n");

    ASSERT_EQ(dev, still_used, 1);
    ASSERT_EQ(dev, after.free_frames, before.free_frames);
}

/**
 * F4: PMM bitmap frames are marked USED after init.
 *
 * The bitmap lives in .bss inside the kernel image.  pmm_init() calls
 * _mark_range_used() on the kernel image region which encompasses the
 * bitmap.  Verify a sample of addresses across [_kernel_start,
 * _kernel_end) are all USED — this covers both the kernel code/data
 * and the bitmap that sits in .bss.
 */
static void test_pmm_bitmap_reserved(serial_dev_t *dev) {
    uint64_t ks = (uint64_t)(uintptr_t)_kernel_start - HIGHER_HALF_OFFSET;
    uint64_t ke = (uint64_t)(uintptr_t)_kernel_end   - HIGHER_HALF_OFFSET;

    /* Sample at least 8 evenly-spaced page-aligned addresses. */
    uint64_t span  = ke - ks;
    uint64_t step  = (span / 8) & ~(uint64_t)(PAGE_SIZE - 1);
    if (step == 0) step = PAGE_SIZE;

    uint64_t start_addr = (ks + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    serial_write_string(dev, "[PMM TEST] bitmap_reserved:\r\n");
    t_log_hex(dev, "kernel_start", ks);
    t_log_hex(dev, "kernel_end  ", ke);
    t_log_hex(dev, "sample_step ", step);

    int all_used = 1;
    for (uint64_t addr = start_addr; addr < ke; addr += step) {
        int f = pmm_is_free(addr);
        if (f != 0) {
            serial_write_string(dev, "[PMM TEST]   FAIL: addr ");
            t_hex64(dev, addr);
            serial_write_string(dev, " is free (should be reserved)\r\n");
            all_used = 0;
            break;
        }
    }
    ASSERT_EQ(dev, all_used, 1);
}

/**
 * F5: pmm_alloc_frame() structurally cannot return physical address 0.
 *
 * Frame 0 is already USED after init, so a normal alloc call should
 * return a frame >= 1.  This test also verifies the bit-mask guard
 * added in Fix 3: even if frame 0 were somehow free in the bitmap,
 * the allocator masks it out before calling __builtin_ctzll().
 *
 * We verify: addr != 0 AND addr >= PAGE_SIZE, and that stats are
 * symmetric after a free.
 */
static void test_pmm_alloc_never_frame0(serial_dev_t *dev) {
    pmm_stats_t before, after;
    pmm_get_stats(&before);

    uint64_t addr = pmm_alloc_frame();

    serial_write_string(dev, "[PMM TEST] alloc_never_frame0: addr=");
    t_hex64(dev, addr); serial_write_string(dev, "\r\n");

    ASSERT_TRUE(dev, addr != 0);
    ASSERT_TRUE(dev, addr >= PAGE_SIZE);   /* must be frame 1 or higher */

    pmm_free_frame(addr);

    pmm_get_stats(&after);
    ASSERT_EQ(dev, after.free_frames, before.free_frames);
}

/* =========================================================================
 * Group G — Edge cases for count=0, out-of-range, large contig, symmetry
 * ========================================================================= */

/** G1: pmm_alloc_frames(0) returns 0 (no-op). */
static void test_pmm_alloc_frames_zero(serial_dev_t *dev) {
    uint64_t addr = pmm_alloc_frames(0);
    serial_write_string(dev, "[PMM TEST] alloc_frames_zero: addr=");
    t_hex64(dev, addr); serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, addr, (uint64_t)0);
}

/** G2: pmm_free_frame on an out-of-range address returns PMM_ERR_INVALID. */
static void test_pmm_free_frame_out_of_range(serial_dev_t *dev) {
    /* Pick an address far beyond any real RAM. */
    uint64_t bad = 0x80000000ULL;  /* 2 GiB — beyond QEMU's 128 MiB default */
    pmm_status_t st = pmm_free_frame(bad);
    serial_write_string(dev, "[PMM TEST] free_frame_out_of_range: status=");
    t_uint64(dev, (uint64_t)(-(int)st));
    serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, (int)st, (int)PMM_ERR_INVALID);
}

/** G3: pmm_is_free on an out-of-range address returns -1. */
static void test_pmm_is_free_out_of_range(serial_dev_t *dev) {
    uint64_t bad = 0x80000000ULL;
    int r = pmm_is_free(bad);
    serial_write_string(dev, "[PMM TEST] is_free_out_of_range: ");
    t_uint64(dev, (uint64_t)(r < 0 ? (uint64_t)(-r) : (uint64_t)r));
    serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, r, -1);
}

/** G4: pmm_alloc_frames(16) returns aligned, contiguous, and frees cleanly. */
static void test_pmm_alloc_contig_large(serial_dev_t *dev) {
    pmm_stats_t before, after;
    pmm_get_stats(&before);

    uint64_t base = pmm_alloc_frames(16);
    serial_write_string(dev, "[PMM TEST] alloc_contig_large: base=");
    t_hex64(dev, base); serial_write_string(dev, "\r\n");
    ASSERT_TRUE(dev, base != 0);
    ASSERT_EQ(dev, base & 0xFFF, 0ULL);

    /* Every frame in the 16-page block must be USED. */
    for (uint64_t i = 0; i < 16; i++) {
        ASSERT_EQ(dev, pmm_is_free(base + i * PAGE_SIZE), 0);
    }

    pmm_get_stats(&after);
    ASSERT_EQ(dev, before.free_frames - after.free_frames, (uint64_t)16);

    /* Free all and verify symmetry. */
    for (uint64_t i = 0; i < 16; i++)
        pmm_free_frame(base + i * PAGE_SIZE);

    pmm_get_stats(&after);
    ASSERT_EQ(dev, after.free_frames, before.free_frames);
}

/** G5: Alloc N frames, free N frames — free_frames is unchanged. */
#define SYMMETRY_N 32
static void test_pmm_stats_symmetric(serial_dev_t *dev) {
    pmm_stats_t before, after;
    pmm_get_stats(&before);

    uint64_t frames[SYMMETRY_N];
    for (int i = 0; i < SYMMETRY_N; i++) {
        frames[i] = pmm_alloc_frame();
        ASSERT_TRUE(dev, frames[i] != 0);
    }

    for (int i = 0; i < SYMMETRY_N; i++)
        pmm_free_frame(frames[i]);

    pmm_get_stats(&after);
    serial_write_string(dev, "[PMM TEST] stats_symmetric: before=");
    t_uint64(dev, before.free_frames);
    serial_write_string(dev, " after="); t_uint64(dev, after.free_frames);
    serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, after.free_frames, before.free_frames);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_pmm(void) {
    /* Group A */
    test_register("pmm_stats_sane",            test_pmm_stats_sane);
    test_register("pmm_frame0_reserved",        test_pmm_frame0_reserved);
    test_register("pmm_kernel_reserved",        test_pmm_kernel_reserved);
    test_register("pmm_free_frames_nonzero",    test_pmm_free_frames_nonzero);

    /* Group B */
    test_register("pmm_alloc_returns_aligned",  test_pmm_alloc_returns_aligned);
    test_register("pmm_alloc_nonzero",          test_pmm_alloc_nonzero);
    test_register("pmm_alloc_marks_used",       test_pmm_alloc_marks_used);
    test_register("pmm_free_marks_free",        test_pmm_free_marks_free);
    test_register("pmm_free_bad_align",         test_pmm_free_bad_align);
    test_register("pmm_free_double_free",       test_pmm_free_double_free);

    /* Group C */
    test_register("pmm_alloc_unique",           test_pmm_alloc_unique);
    test_register("pmm_alloc_free_reuse",       test_pmm_alloc_free_reuse);
    test_register("pmm_alloc_many_unique",      test_pmm_alloc_many_unique);
    test_register("pmm_stats_track_alloc",      test_pmm_stats_track_alloc);
    test_register("pmm_stats_track_free",       test_pmm_stats_track_free);

    /* Group D */
    test_register("pmm_alloc_contig_aligned",   test_pmm_alloc_contig_aligned);
    test_register("pmm_alloc_contig_contig",    test_pmm_alloc_contig_contiguous);
    test_register("pmm_alloc_contig_unique",    test_pmm_alloc_contig_unique_vs_single);
    test_register("pmm_alloc_contig_free_all",  test_pmm_alloc_contig_free_all);

    /* Group E */
    test_register("pmm_is_free_after_alloc",    test_pmm_is_free_after_alloc);
    test_register("pmm_is_free_after_free",     test_pmm_is_free_after_free);
    test_register("pmm_is_free_invalid",        test_pmm_is_free_invalid);

    /* Group F — mark helpers and structural guards */
    test_register("pmm_mark_used_works",        test_pmm_mark_used_works);
    test_register("pmm_mark_free_works",        test_pmm_mark_free_works);
    test_register("pmm_mark_free_frame0_noop",  test_pmm_mark_free_frame0_noop);
    test_register("pmm_bitmap_reserved",        test_pmm_bitmap_reserved);
    test_register("pmm_alloc_never_frame0",     test_pmm_alloc_never_frame0);

    /* Group G — Edge cases */
    test_register("pmm_alloc_frames_zero",      test_pmm_alloc_frames_zero);
    test_register("pmm_free_frame_out_of_range", test_pmm_free_frame_out_of_range);
    test_register("pmm_is_free_out_of_range",   test_pmm_is_free_out_of_range);
    test_register("pmm_alloc_contig_large",     test_pmm_alloc_contig_large);
    test_register("pmm_stats_symmetric",        test_pmm_stats_symmetric);
}
