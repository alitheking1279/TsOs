/**
 * @file test_idt.c
 * @brief IDT, PIC, and ISR hardware-state verification tests.
 *
 * All tests execute after idt_init() has been called, so the IDTR, PIC,
 * and ISR handler table are live.  Each test reads actual CPU state (sidt,
 * inb) or exercises the interrupt machinery via software interrupts (int N).
 *
 * Tests registered (16 total):
 *
 *   IDT structure (6):
 *     idt_limit_correct         — IDTR.limit == 4095
 *     idt_base_correct          — IDTR.base  == idt_get_address()
 *     idt_gate_present          — vector 0 gate has Present bit set
 *     idt_gate_type_interrupt   — vector 0 gate type == 0xE (interrupt)
 *     idt_nmi_trap_gate         — vector 2 gate type == 0xF (trap)
 *     idt_doublefault_ist1      — vector 8 gate IST index == 1
 *
 *   PIC (4):
 *     pic_imr_master_masked     — master IMR == 0xFF after init
 *     pic_imr_slave_masked      — slave IMR == 0xFF after init
 *     pic_mask_unmask           — mask/unmask IRQ0, verify IMR changes
 *     pic_eoi_no_crash          — send EOI for master + slave, no hang
 *
 *   ISR handler dispatch (6):
 *     isr_handler_called        — register + trigger, verify handler fires
 *     isr_no_handler_no_crash   — trigger int with no handler, no crash
 *     isr_frame_vector_match    — handler inspects frame->vector
 *     isr_frame_cs_kernel       — handler inspects frame->cs == 0x08
 *     isr_frame_rip_valid       — handler inspects frame->rip >= 1MiB
 *     isr_divide_by_zero        — int $0 fires vector 0 handler
 */

#include "test.h"
#include "../kernel/idt.h"
#include "../kernel/isr.h"
#include "../kernel/pic.h"
#include "../kernel/gdt.h"
#include <stdint.h>

/* =========================================================================
 * Helper: read the live IDTR via sidt
 *
 * sidt stores 10 bytes: 2-byte limit then 8-byte base (both little-endian).
 * The struct must be packed to prevent compiler padding.
 * ========================================================================= */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

static inline void read_idtr(idtr_t *out) {
    __asm__ volatile ("sidt %0" : "=m"(*out));
}

/* =========================================================================
 * Shared test state for ISR handler tests
 *
 * Volatile to prevent the compiler from optimizing away reads/writes
 * across the int $N boundary (the compiler cannot see through an
 * interrupt gate).
 * ========================================================================= */
static volatile int     g_handler_called;
static volatile uint8_t g_captured_vector;
static volatile uint64_t g_captured_cs;
static volatile uint64_t g_captured_rip;
static volatile uint8_t g_expected_vector;

/* =========================================================================
 * Test handler functions — each captures specific frame fields
 * ========================================================================= */

/** Generic handler: sets the called flag and captures vector. */
static void test_isr_handler(interrupt_frame_t *frame) {
    (void)frame;
    g_handler_called = 1;
    g_captured_vector = (uint8_t)frame->vector;
}

/** Vector-specific handler: captures vector and verifies it matches expected. */
static void test_isr_vector_handler(interrupt_frame_t *frame) {
    g_handler_called = 1;
    g_captured_vector = (uint8_t)frame->vector;
    if (frame->vector != g_expected_vector) {
        /* Set vector to 0xFF as a signal that the assertion failed */
        g_captured_vector = 0xFF;
    }
}

/** Handler that captures CS for kernel-CS verification. */
static void test_isr_cs_handler(interrupt_frame_t *frame) {
    g_handler_called = 1;
    g_captured_cs = frame->cs;
}

/** Handler that captures RIP for kernel-text verification. */
static void test_isr_rip_handler(interrupt_frame_t *frame) {
    g_handler_called = 1;
    g_captured_rip = frame->rip;
}

/* =========================================================================
 * IDT structure tests (5 tests)
 * ========================================================================= */

/** Test 1: IDTR limit == IDT_ENTRIES * 16 - 1 = 4095. */
static void test_idt_limit_correct(serial_dev_t *dev) {
    idtr_t idtr;
    read_idtr(&idtr);
    uint16_t expected = (uint16_t)(IDT_ENTRIES * sizeof(idt_entry_t) - 1);
    ASSERT_EQ(dev, idtr.limit, expected);
}

/** Test 2: IDTR base == idt_get_address(). */
static void test_idt_base_correct(serial_dev_t *dev) {
    idtr_t idtr;
    read_idtr(&idtr);
    ASSERT_EQ(dev, idtr.base, idt_get_address());
}

/** Test 3: Vector 0 gate descriptor has Present bit (bit 7 of byte 5) set. */
static void test_idt_gate_present(serial_dev_t *dev) {
    idtr_t idtr;
    read_idtr(&idtr);

    /* byte 5 of each 16-byte entry contains type_attr (P | DPL | type) */
    volatile const uint8_t *idt_bytes = (volatile const uint8_t *)idtr.base;
    uint8_t byte5 = idt_bytes[0 * 16 + 5];  /* vector 0, byte 5 */

    ASSERT_TRUE(dev, (byte5 & 0x80) != 0);  /* P bit (bit 7) must be set */
}

/** Test 4: Vector 0 gate is an interrupt gate (type 0xE in low nibble of byte 5). */
static void test_idt_gate_type_interrupt(serial_dev_t *dev) {
    idtr_t idtr;
    read_idtr(&idtr);

    volatile const uint8_t *idt_bytes = (volatile const uint8_t *)idtr.base;
    uint8_t byte5 = idt_bytes[0 * 16 + 5];

    /* type_attr = P(1) | DPL(00) | 0 | Type(E) = 0x8E */
    ASSERT_EQ(dev, (uint32_t)byte5, (uint32_t)0x8E);
}

/** Test 4b: Vector 2 (NMI) gate is a trap gate (type 0xF) so NMI
 *            delivery is not blocked by the IF flag. */
static void test_idt_nmi_trap_gate(serial_dev_t *dev) {
    idtr_t idtr;
    read_idtr(&idtr);

    volatile const uint8_t *idt_bytes = (volatile const uint8_t *)idtr.base;
    uint8_t byte5 = idt_bytes[2 * 16 + 5];

    /* type_attr = P(1) | DPL(00) | 0 | Type(F) = 0x8F */
    ASSERT_EQ(dev, (uint32_t)byte5, (uint32_t)0x8F);
}

/** Test 5: Vector 8 (double fault) gate has IST index == 1. */
static void test_idt_doublefault_ist1(serial_dev_t *dev) {
    idtr_t idtr;
    read_idtr(&idtr);

    volatile const uint8_t *idt_bytes = (volatile const uint8_t *)idtr.base;
    /* byte 4 of vector 8's entry: IST[2:0] in bits 2:0, rest = 0 */
    uint8_t byte4 = idt_bytes[8 * 16 + 4];

    ASSERT_EQ(dev, (uint32_t)(byte4 & 0x07), (uint32_t)1);
}

/* =========================================================================
 * PIC tests (4 tests)
 * ========================================================================= */

/** Test 6: Master PIC IMR == 0xFF (all IRQs masked) after init. */
static void test_pic_imr_master_masked(serial_dev_t *dev) {
    ASSERT_EQ(dev, (uint32_t)pic_get_imr_master(), (uint32_t)0xFF);
}

/** Test 7: Slave PIC IMR == 0xFF (all IRQs masked) after init. */
static void test_pic_imr_slave_masked(serial_dev_t *dev) {
    ASSERT_EQ(dev, (uint32_t)pic_get_imr_slave(), (uint32_t)0xFF);
}

/** Test 8: Mask/unmask IRQ0 changes the master IMR bit 0. */
static void test_pic_mask_unmask(serial_dev_t *dev) {
    /* Unmask IRQ0 (timer) — clear bit 0 in master IMR */
    pic_unmask_irq(0);
    uint8_t imr = pic_get_imr_master();
    ASSERT_TRUE(dev, (imr & 0x01) == 0);  /* bit 0 must be clear */

    /* Mask IRQ0 — set bit 0 in master IMR */
    pic_mask_irq(0);
    imr = pic_get_imr_master();
    ASSERT_EQ(dev, (uint32_t)imr, (uint32_t)0xFF);  /* all masked again */
}

/** Test 9: Sending EOI for various IRQ lines does not crash/hang. */
static void test_pic_eoi_no_crash(serial_dev_t *dev) {
    /* Send EOI for master IRQs (0-7) — no-op since no ISR bit is set,
     * but should not crash or hang. */
    pic_send_eoi(0);
    pic_send_eoi(7);

    /* Send EOI for slave IRQs (8-15) — sends to both slave and master. */
    pic_send_eoi(8);
    pic_send_eoi(15);

    /* If we reach here, no hang occurred. */
    ASSERT_TRUE(dev, 1);
}

/* =========================================================================
 * ISR handler dispatch tests (6 tests)
 * ========================================================================= */

/** Test 10: Register handler on vector 0x80, trigger with int $0x80,
 *            verify handler was called. */
static void test_isr_handler_called(serial_dev_t *dev) {
    g_handler_called = 0;
    isr_register_handler(0x80, test_isr_handler);

    __asm__ volatile ("int $0x80");

    isr_unregister_handler(0x80);
    ASSERT_EQ(dev, (uint32_t)g_handler_called, (uint32_t)1);
}

/** Test 11: Trigger int $0x81 with no handler registered — no crash. */
static void test_isr_no_handler_no_crash(serial_dev_t *dev) {
    /* Vector 0x81 has no handler registered.  isr_common_handler should
     * skip dispatch without dereferencing a NULL pointer. */
    __asm__ volatile ("int $0x81");
    ASSERT_TRUE(dev, 1);  /* reaching this line proves no crash */
}

/** Test 12: Handler verifies frame->vector matches the triggered vector. */
static void test_isr_frame_vector_match(serial_dev_t *dev) {
    g_handler_called = 0;
    g_captured_vector = 0;
    g_expected_vector = 0x82;
    isr_register_handler(0x82, test_isr_vector_handler);

    __asm__ volatile ("int $0x82");

    isr_unregister_handler(0x82);
    ASSERT_EQ(dev, (uint32_t)g_handler_called, (uint32_t)1);
    ASSERT_EQ(dev, (uint32_t)g_captured_vector, (uint32_t)0x82);
}

/** Test 13: Handler verifies frame->cs == GDT_KERNEL_CS_SEL (0x08). */
static void test_isr_frame_cs_kernel(serial_dev_t *dev) {
    g_handler_called = 0;
    g_captured_cs = 0;
    isr_register_handler(0x83, test_isr_cs_handler);

    __asm__ volatile ("int $0x83");

    isr_unregister_handler(0x83);
    ASSERT_EQ(dev, (uint32_t)g_handler_called, (uint32_t)1);
    ASSERT_EQ(dev, (uint32_t)g_captured_cs, (uint32_t)GDT_KERNEL_CS_SEL);
}

/** Test 14: Handler verifies frame->rip >= 0x100000 (kernel text base). */
static void test_isr_frame_rip_valid(serial_dev_t *dev) {
    g_handler_called = 0;
    g_captured_rip = 0;
    isr_register_handler(0x84, test_isr_rip_handler);

    __asm__ volatile ("int $0x84");

    isr_unregister_handler(0x84);
    ASSERT_EQ(dev, (uint32_t)g_handler_called, (uint32_t)1);
    /* Kernel is linked at 1 MiB (linker.ld: . = 1M) */
    ASSERT_TRUE(dev, g_captured_rip >= 0x100000);
}

/** Test 15: Trigger divide-by-zero (int $0) — verify vector 0 handler fires. */
static void test_isr_divide_by_zero(serial_dev_t *dev) {
    g_handler_called = 0;
    g_captured_vector = 0;
    isr_register_handler(0, test_isr_handler);

    /* int $0 triggers vector 0 (same as #DE exception).
     * The handler fires with interrupts disabled (interrupt gate),
     * sets our flag, then returns via iretq. */
    __asm__ volatile ("int $0");

    isr_unregister_handler(0);
    ASSERT_EQ(dev, (uint32_t)g_handler_called, (uint32_t)1);
    ASSERT_EQ(dev, (uint32_t)g_captured_vector, (uint32_t)0);
}

/* =========================================================================
 * Registration
 * ========================================================================= */
void test_register_idt(void) {
    /* IDT structure (6) */
    test_register("idt_limit_correct",       test_idt_limit_correct);
    test_register("idt_base_correct",        test_idt_base_correct);
    test_register("idt_gate_present",        test_idt_gate_present);
    test_register("idt_gate_type_interrupt", test_idt_gate_type_interrupt);
    test_register("idt_nmi_trap_gate",       test_idt_nmi_trap_gate);
    test_register("idt_doublefault_ist1",    test_idt_doublefault_ist1);

    /* PIC (4) */
    test_register("pic_imr_master_masked",   test_pic_imr_master_masked);
    test_register("pic_imr_slave_masked",    test_pic_imr_slave_masked);
    test_register("pic_mask_unmask",         test_pic_mask_unmask);
    test_register("pic_eoi_no_crash",        test_pic_eoi_no_crash);

    /* ISR handler dispatch (6) */
    test_register("isr_handler_called",      test_isr_handler_called);
    test_register("isr_no_handler_no_crash", test_isr_no_handler_no_crash);
    test_register("isr_frame_vector_match",  test_isr_frame_vector_match);
    test_register("isr_frame_cs_kernel",     test_isr_frame_cs_kernel);
    test_register("isr_frame_rip_valid",     test_isr_frame_rip_valid);
    test_register("isr_divide_by_zero",      test_isr_divide_by_zero);
}
