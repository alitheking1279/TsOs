#include "test.h"
#include "kernel/spinlock.h"

/* Tests for spinlock primitives. */

static void test_spinlock_init(serial_dev_t *dev)
{
    spinlock_t lock;
    spinlock_init(&lock);
    ASSERT_EQ(dev, lock.locked, (uint32_t)0);
}

static void test_spinlock_acquire_release(serial_dev_t *dev)
{
    spinlock_t lock;
    spinlock_init(&lock);
    uint64_t rflags = spin_lock(&lock);
    ASSERT_EQ(dev, lock.locked, (uint32_t)1);
    spin_unlock(&lock, rflags);
    ASSERT_EQ(dev, lock.locked, (uint32_t)0);
}

static void test_spinlock_trylock(serial_dev_t *dev)
{
    spinlock_t lock;
    spinlock_init(&lock);

    /* Should succeed on unlocked. */
    uint64_t rflags1 = spin_trylock(&lock);
    ASSERT_TRUE(dev, rflags1 != 0);
    ASSERT_EQ(dev, lock.locked, (uint32_t)1);

    /* Should fail on locked. */
    uint64_t rflags2 = spin_trylock(&lock);
    ASSERT_EQ(dev, rflags2, (uint64_t)0);

    spin_unlock(&lock, rflags1);
    ASSERT_EQ(dev, lock.locked, (uint32_t)0);
}

static void test_spinlock_reentrant_fail(serial_dev_t *dev)
{
    spinlock_t lock;
    spinlock_init(&lock);
    uint64_t rflags = spin_lock(&lock);
    /* Second lock attempt would deadlock — just verify state and unlock. */
    ASSERT_EQ(dev, lock.locked, (uint32_t)1);
    spin_unlock(&lock, rflags);
}

void test_register_spinlock(void)
{
    test_register("spinlock_init", test_spinlock_init);
    test_register("spinlock_acquire_release", test_spinlock_acquire_release);
    test_register("spinlock_trylock", test_spinlock_trylock);
    test_register("spinlock_reentrant_fail", test_spinlock_reentrant_fail);
}
