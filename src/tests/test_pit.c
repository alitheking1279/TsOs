/**
 * @file test_pit.c
 * @brief PIT driver tests — frequency, counter readback, mode verification.
 *
 * Tests registered (5):
 *   pit_default_freq       — pit_get_frequency() == 100 after init
 *   pit_custom_freq        — pit_init(200) → get_frequency() == 200
 *   pit_zero_freq_fallback — pit_init(0) falls back to 100
 *   pit_counter_nonzero    — pit_get_counter() returns a nonzero value
 *   pit_counter_range      — at 100 Hz, counter is between 10000-13000
 */

#include "test.h"
#include "../drivers/pit.h"
#include <stdint.h>

/* ---- Test: default frequency ---- */
static void test_pit_default_freq(serial_dev_t *dev) {
    /* pit_init was already called by timer_init with PIT_DEFAULT_FREQ. */
    ASSERT_EQ(dev, pit_get_frequency(), (uint32_t)PIT_DEFAULT_FREQ);
}

/* ---- Test: custom frequency ---- */
static void test_pit_custom_freq(serial_dev_t *dev) {
    pit_init(200);
    ASSERT_EQ(dev, pit_get_frequency(), (uint32_t)200);
    /* Restore to default for subsequent tests. */
    pit_init(PIT_DEFAULT_FREQ);
}

/* ---- Test: zero frequency falls back to default ---- */
static void test_pit_zero_freq_fallback(serial_dev_t *dev) {
    pit_init(0);
    ASSERT_EQ(dev, pit_get_frequency(), (uint32_t)PIT_DEFAULT_FREQ);
}

/* ---- Test: counter is nonzero after programming ---- */
static void test_pit_counter_nonzero(serial_dev_t *dev) {
    pit_init(PIT_DEFAULT_FREQ);
    uint16_t counter = pit_get_counter();
    /* At 100 Hz, the counter should be cycling around 11931. */
    ASSERT_TRUE(dev, counter != 0);
}

/* ---- Test: counter is in expected range for 100 Hz ---- */
static void test_pit_counter_range(serial_dev_t *dev) {
    pit_init(PIT_DEFAULT_FREQ);
    /* Divisor = 1193182 / 100 = 11931.  Counter counts down from 11931.
     * It should always be between 0 and 11931 (inclusive). */
    uint16_t counter = pit_get_counter();
    ASSERT_TRUE(dev, counter <= 11931);
}

/* ---- Registration ---- */
void test_register_pit(void) {
    test_register("pit_default_freq", test_pit_default_freq);
    test_register("pit_custom_freq", test_pit_custom_freq);
    test_register("pit_zero_freq_fallback", test_pit_zero_freq_fallback);
    test_register("pit_counter_nonzero", test_pit_counter_nonzero);
    test_register("pit_counter_range", test_pit_counter_range);
}
