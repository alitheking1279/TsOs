/**
 * @file test_gdt.c
 * @brief GDT and TSS hardware-state verification tests.
 *
 * All tests execute after gdt_init() has been called, so the GDTR and TR
 * are live.  Each test reads actual CPU state (sgdt, mov %%cs) or inspects
 * the GDT array directly via the GDTR base pointer — we test what the
 * hardware loaded, not just what the C code believes it wrote.
 *
 * Tests registered:
 *   gdt_limit_correct       — GDTR.limit == GDT_ENTRY_COUNT * 8 - 1 = 55
 *   gdt_base_correct        — GDTR.base  == gdt_get_address()
 *   gdt_cs_loads            — CS register == GDT_KERNEL_CS_SEL after far-return
 *   gdt_user_segments_exist — GDT[3] DPL==3, GDT[4] DPL==3
 *   gdt_tss_base_correct    — TSS base decoded from GDT[5+6] == gdt_get_tss_address()
 */

#include "test.h"
#include "../kernel/gdt.h"
#include <stdint.h>

/* =========================================================================
 * Helper: read the live GDTR via sgdt.
 *
 * sgdt stores 10 bytes: 2-byte limit then 8-byte base (both little-endian).
 * The struct must be packed to prevent compiler padding between the fields.
 * ========================================================================= */
typedef struct {
    uint16_t limit; /**< GDT byte length minus 1. */
    uint64_t base;  /**< Linear address of GDT[0]. */
} __attribute__((packed)) gdtr_t;

static inline void read_gdtr(gdtr_t *out) {
    __asm__ volatile ("sgdt %0" : "=m"(*out));
}

/* =========================================================================
 * Test 1: GDTR limit
 *
 * Expected: GDT_ENTRY_COUNT * 8 - 1 = 7 * 8 - 1 = 55.
 * A wrong limit means the CPU either accepts illegal selectors or rejects
 * valid ones with #GP.
 * ========================================================================= */
static void test_gdt_limit_correct(serial_dev_t *dev) {
    gdtr_t gdtr;
    read_gdtr(&gdtr);
    uint16_t expected = (uint16_t)(GDT_ENTRY_COUNT * 8U - 1U);
    ASSERT_EQ(dev, gdtr.limit, expected);
}

/* =========================================================================
 * Test 2: GDTR base matches internal GDT array address.
 *
 * Verifies lgdt received a pointer to g_gdt[], not a stale boot GDT or
 * stack-allocated temporary.
 * ========================================================================= */
static void test_gdt_base_correct(serial_dev_t *dev) {
    gdtr_t gdtr;
    read_gdtr(&gdtr);
    ASSERT_EQ(dev, gdtr.base, gdt_get_address());
}

/* =========================================================================
 * Test 3: CS register equals GDT_KERNEL_CS_SEL (0x08).
 *
 * Verifies the far-return in gdt_flush() correctly reloaded CS.  The test
 * executing at all proves we didn't triple-fault; the value check proves
 * the selector is exact.
 * ========================================================================= */
static void test_gdt_cs_loads(serial_dev_t *dev) {
    uint16_t cs = 0;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    ASSERT_EQ(dev, (uint32_t)cs, (uint32_t)GDT_KERNEL_CS_SEL);
}

/* =========================================================================
 * Test 4: User-mode segment descriptors carry DPL == 3.
 *
 * Reads the live GDT directly and extracts the DPL field from the access
 * byte of entries [3] (user data) and [4] (user code).
 *
 * Note: user data is at index 3 and user code at index 4 by design —
 * SYSRET derives SS = STAR[63:48]+8 and CS = STAR[63:48]+16, so the data
 * descriptor must sit one index below the code descriptor.
 *
 * In the 8-byte descriptor, the access byte lives at bits [47:40].  Within
 * that byte, DPL occupies bits [6:5].  Equivalently, in the full 64-bit
 * descriptor word, DPL = bits [46:45].
 * ========================================================================= */
static void test_gdt_user_segments_exist(serial_dev_t *dev) {
    gdtr_t gdtr;
    read_gdtr(&gdtr);

    /* Cast to volatile to prevent the compiler caching stale reads. */
    volatile const uint64_t *gdt = (volatile const uint64_t *)gdtr.base;

    /* Access byte = bits [47:40]; DPL = access_byte[6:5]. */
    uint8_t access3 = (uint8_t)((gdt[3] >> 40) & 0xFFU);
    uint8_t dpl3    = (uint8_t)((access3 >> 5) & 0x3U);

    uint8_t access4 = (uint8_t)((gdt[4] >> 40) & 0xFFU);
    uint8_t dpl4    = (uint8_t)((access4 >> 5) & 0x3U);

    ASSERT_EQ(dev, (uint32_t)dpl3, 3U);
    ASSERT_EQ(dev, (uint32_t)dpl4, 3U);
}

/* =========================================================================
 * Test 5: TSS base address encoded in GDT entries [5]+[6] is correct.
 *
 * Decodes the four non-contiguous base-address sub-fields from the two-slot
 * 64-bit TSS system descriptor and compares the reconstructed address
 * against gdt_get_tss_address().
 *
 * Note: after tss_flush() (ltr), the CPU promotes the TSS type field from
 * 0x9 (Available) to 0xB (Busy) — only the type bits change, not the base
 * address fields, so decoding is still accurate after LTR runs.
 *
 * Field map (Intel SDM Vol.3A §7.2.3):
 *   GDT[5] bits [31:16] = base[15:0]
 *   GDT[5] bits [39:32] = base[23:16]
 *   GDT[5] bits [63:56] = base[31:24]
 *   GDT[6] bits [31:0]  = base[63:32]
 * ========================================================================= */
static void test_gdt_tss_base_correct(serial_dev_t *dev) {
    gdtr_t gdtr;
    read_gdtr(&gdtr);

    volatile const uint64_t *gdt = (volatile const uint64_t *)gdtr.base;

    uint64_t low  = gdt[5];
    uint64_t high = gdt[6];

    uint64_t decoded_base =
        ((low  >> 16) & 0x000000000000FFFFULL)        |  /* base[15:0]  */
        (((low >> 32) & 0x00000000000000FFULL) << 16) |  /* base[23:16] */
        (((low >> 56) & 0x00000000000000FFULL) << 24) |  /* base[31:24] */
        ((high & 0x00000000FFFFFFFFULL) << 32);           /* base[63:32] */

    ASSERT_EQ(dev, decoded_base, gdt_get_tss_address());
}

/* =========================================================================
 * Registration
 * ========================================================================= */
void test_register_gdt(void) {
    test_register("gdt_limit_correct",       test_gdt_limit_correct);
    test_register("gdt_base_correct",        test_gdt_base_correct);
    test_register("gdt_cs_loads",            test_gdt_cs_loads);
    test_register("gdt_user_segments_exist", test_gdt_user_segments_exist);
    test_register("gdt_tss_base_correct",    test_gdt_tss_base_correct);
}
