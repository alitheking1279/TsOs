/**
 * @file test_spinlock_rflags.c
 * @brief Spinlock RFLAGS save/restore tests — verifies SMP-safe design
 *        where RFLAGS is returned on the caller's stack instead of stored
 *        in the lock struct.
 *
 * Tests registered (4):
 *   spinlock_rflags_saved         — acquire returns correct RFLAGS
 *   spinlock_rflags_restored      — RFLAGS is correctly restored on release
 *   spinlock_trylock_rflags_save  — trylock also returns RFLAGS
 *   spinlock_nested_interrupts    — interrupts stay disabled during critical section
 */

#include "test.h"
#include "../kernel/spinlock.h"
#include <stdint.h>

/* ---- Helper: read RFLAGS ---- */
static inline uint64_t read_rflags(void) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags));
    return rflags;
}

/* ---- Test: acquire returns RFLAGS with correct IF bit ---- */
static void test_spinlock_rflags_saved(serial_dev_t *dev) {
    spinlock_t lock;
    spinlock_init(&lock);

    uint64_t rflags_before = read_rflags();

    uint64_t saved = spin_lock(&lock);

    /* Acquired rflags should have the same IF bit as pre-lock rflags. */
    ASSERT_EQ(dev, (saved >> 9) & 1, (rflags_before >> 9) & 1);

    spin_unlock(&lock, saved);
}

/* ---- Test: RFLAGS matches saved value on release ---- */
static void test_spinlock_rflags_restored(serial_dev_t *dev) {
    spinlock_t lock;
    spinlock_init(&lock);

    /* Capture RFLAGS before locking. */
    uint64_t rflags_before = read_rflags();

    uint64_t saved = spin_lock(&lock);
    spin_unlock(&lock, saved);

    /* After unlock, RFLAGS should match the pre-lock value (same IF bit). */
    uint64_t rflags_after = read_rflags();
    ASSERT_EQ(dev, (rflags_before >> 9) & 1, (rflags_after >> 9) & 1);
}

/* ---- Test: trylock also returns RFLAGS ---- */
static void test_spinlock_trylock_rflags_save(serial_dev_t *dev) {
    spinlock_t lock;
    spinlock_init(&lock);

    uint64_t rflags_before = read_rflags();

    uint64_t rflags = spin_trylock(&lock);
    ASSERT_TRUE(dev, rflags != 0);

    /* saved rflags should have the same IF bit as pre-lock rflags. */
    ASSERT_EQ(dev, (rflags >> 9) & 1, (rflags_before >> 9) & 1);

    spin_unlock(&lock, rflags);
}

/* ---- Test: interrupts stay disabled during critical section ---- */
static void test_spinlock_nested_interrupts(serial_dev_t *dev) {
    spinlock_t lock;
    spinlock_init(&lock);

    uint64_t rflags = spin_lock(&lock);

    /* Read RFLAGS while holding the lock. IF should be clear (interrupts disabled). */
    uint64_t current_rflags = read_rflags();
    uint64_t if_bit = (current_rflags >> 9) & 1;

    spin_unlock(&lock, rflags);

    /* IF should have been 0 while the lock was held. */
    ASSERT_EQ(dev, if_bit, (uint64_t)0);
}

/* ---- Registration ---- */
void test_register_spinlock_rflags(void) {
    test_register("spinlock_rflags_saved", test_spinlock_rflags_saved);
    test_register("spinlock_rflags_restored", test_spinlock_rflags_restored);
    test_register("spinlock_trylock_rflags_save", test_spinlock_trylock_rflags_save);
    test_register("spinlock_nested_interrupts", test_spinlock_nested_interrupts);
}
