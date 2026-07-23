/**
 * @file test_mlfq.c
 * @brief MLFQ scheduler tests — enqueue, pick, decay, boost, priority ordering.
 *
 * Tests exercise the MLFQ data structures directly (no context_switch).
 * Tasks are created as kernel tasks and enqueued/dequeued to verify
 * priority ordering, decay, and boost behavior.
 *
 * Tests registered (8):
 *   mlfq_init_empty       — after init, all queues empty, bitmap=0
 *   mlfq_enqueue_level0   — enqueue at L0, pick returns same task
 *   mlfq_priority_order   — L0 task picked before L1 task
 *   mlfq_decay_demotes    — decay moves task L0 -> L1 -> L2 -> L3
 *   mlfq_decay_clamps     — decay at L3 stays at L3
 *   mlfq_boost_promotes   — boost moves all tasks to L0
 *   mlfq_dequeue_removes  — dequeue removes task, queue empty
 *   mlfq_total_count_ok   — total_count tracks enqueues/dequeues
 */

#include "test.h"
#include "../kernel/mlfq.h"
#include "../kernel/task.h"
#include <stdint.h>

/* Stub entry for test tasks (never actually context-switched into). */
static void mlfq_stub_entry(void) {
    while (1) { asm volatile ("hlt"); }
}

/* ---- Test: MLFQ init leaves queues empty ---- */
static void test_mlfq_init_empty(serial_dev_t *dev) {
    mlfq_init();
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)0);
    ASSERT_NULL(dev, mlfq_pick_highest());
}

/* ---- Test: enqueue at L0, pick returns it ---- */
static void test_mlfq_enqueue_level0(serial_dev_t *dev) {
    mlfq_init();
    task_t *t = task_create("mlfq_t0", mlfq_stub_entry, true);
    ASSERT_NOT_NULL(dev, t);

    mlfq_enqueue(t, 0);
    task_t *picked = mlfq_pick_highest();
    ASSERT_EQ(dev, (uint64_t)picked, (uint64_t)t);
    ASSERT_EQ(dev, t->mlfq_level, (int)0);

    mlfq_dequeue(t);
    task_destroy(t);
}

/* ---- Test: L0 task is picked before L1 task ---- */
static void test_mlfq_priority_order(serial_dev_t *dev) {
    mlfq_init();
    task_t *t0 = task_create("mlfq_lo", mlfq_stub_entry, true);
    task_t *t1 = task_create("mlfq_hi", mlfq_stub_entry, true);
    ASSERT_NOT_NULL(dev, t0);
    ASSERT_NOT_NULL(dev, t1);

    /* Enqueue L1 first, then L0.  L0 should be picked first. */
    mlfq_enqueue(t0, 1);
    mlfq_enqueue(t1, 0);

    task_t *picked = mlfq_pick_highest();
    ASSERT_EQ(dev, (uint64_t)picked, (uint64_t)t1);  /* L0 wins */
    ASSERT_EQ(dev, picked->mlfq_level, (int)0);

    mlfq_dequeue(t1);
    picked = mlfq_pick_highest();
    ASSERT_EQ(dev, (uint64_t)picked, (uint64_t)t0);  /* L1 is next */

    mlfq_dequeue(t0);
    task_destroy(t0);
    task_destroy(t1);
}

/* ---- Test: decay demotes through levels ---- */
static void test_mlfq_decay_demotes(serial_dev_t *dev) {
    mlfq_init();
    task_t *t = task_create("mlfq_d", mlfq_stub_entry, true);
    ASSERT_NOT_NULL(dev, t);

    mlfq_enqueue(t, 0);
    ASSERT_EQ(dev, t->mlfq_level, (int)0);

    mlfq_decay(t);
    ASSERT_EQ(dev, t->mlfq_level, (int)1);

    mlfq_decay(t);
    ASSERT_EQ(dev, t->mlfq_level, (int)2);

    mlfq_decay(t);
    ASSERT_EQ(dev, t->mlfq_level, (int)3);

    mlfq_dequeue(t);
    task_destroy(t);
}

/* ---- Test: decay at L3 stays at L3 ---- */
static void test_mlfq_decay_clamps(serial_dev_t *dev) {
    mlfq_init();
    task_t *t = task_create("mlfq_cl", mlfq_stub_entry, true);
    ASSERT_NOT_NULL(dev, t);

    mlfq_enqueue(t, 3);
    ASSERT_EQ(dev, t->mlfq_level, (int)3);

    /* Decay should keep it at L3. */
    mlfq_decay(t);
    ASSERT_EQ(dev, t->mlfq_level, (int)3);
    ASSERT_EQ(dev, t->remaining_ticks, (int64_t)16);  /* L3 quanta = 16 */

    mlfq_dequeue(t);
    task_destroy(t);
}

/* ---- Test: boost promotes all tasks to L0 ---- */
static void test_mlfq_boost_promotes(serial_dev_t *dev) {
    mlfq_init();
    task_t *t1 = task_create("mlfq_b1", mlfq_stub_entry, true);
    task_t *t2 = task_create("mlfq_b2", mlfq_stub_entry, true);
    task_t *t3 = task_create("mlfq_b3", mlfq_stub_entry, true);
    ASSERT_NOT_NULL(dev, t1);
    ASSERT_NOT_NULL(dev, t2);
    ASSERT_NOT_NULL(dev, t3);

    /* Place tasks at levels 1, 2, 3. */
    mlfq_enqueue(t1, 1);
    mlfq_enqueue(t2, 2);
    mlfq_enqueue(t3, 3);
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)3);

    /* Boost should move all to L0. */
    mlfq_boost_all();

    ASSERT_EQ(dev, t1->mlfq_level, (int)0);
    ASSERT_EQ(dev, t2->mlfq_level, (int)0);
    ASSERT_EQ(dev, t3->mlfq_level, (int)0);

    /* All should have L0 quanta (2 ticks). */
    ASSERT_EQ(dev, t1->remaining_ticks, (int64_t)2);
    ASSERT_EQ(dev, t2->remaining_ticks, (int64_t)2);
    ASSERT_EQ(dev, t3->remaining_ticks, (int64_t)2);

    /* Still 3 tasks total. */
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)3);

    mlfq_dequeue(t1);
    mlfq_dequeue(t2);
    mlfq_dequeue(t3);
    task_destroy(t1);
    task_destroy(t2);
    task_destroy(t3);
}

/* ---- Test: dequeue removes task ---- */
static void test_mlfq_dequeue_removes(serial_dev_t *dev) {
    mlfq_init();
    task_t *t = task_create("mlfq_rm", mlfq_stub_entry, true);
    ASSERT_NOT_NULL(dev, t);

    mlfq_enqueue(t, 2);
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)1);

    mlfq_dequeue(t);
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)0);
    ASSERT_NULL(dev, mlfq_pick_highest());

    task_destroy(t);
}

/* ---- Test: total_count tracks operations ---- */
static void test_mlfq_total_count_ok(serial_dev_t *dev) {
    mlfq_init();
    task_t *a = task_create("mlfq_a", mlfq_stub_entry, true);
    task_t *b = task_create("mlfq_b", mlfq_stub_entry, true);
    task_t *c = task_create("mlfq_c", mlfq_stub_entry, true);
    ASSERT_NOT_NULL(dev, a);
    ASSERT_NOT_NULL(dev, b);
    ASSERT_NOT_NULL(dev, c);

    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)0);
    mlfq_enqueue(a, 0);
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)1);
    mlfq_enqueue(b, 1);
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)2);
    mlfq_enqueue(c, 2);
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)3);

    mlfq_dequeue(b);
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)2);
    mlfq_dequeue(a);
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)1);
    mlfq_dequeue(c);
    ASSERT_EQ(dev, mlfq_total_count(), (uint32_t)0);

    task_destroy(a);
    task_destroy(b);
    task_destroy(c);
}

/* ---- Registration ---- */
void test_register_mlfq(void) {
    test_register("mlfq_init_empty",     test_mlfq_init_empty);
    test_register("mlfq_enqueue_level0", test_mlfq_enqueue_level0);
    test_register("mlfq_priority_order", test_mlfq_priority_order);
    test_register("mlfq_decay_demotes",  test_mlfq_decay_demotes);
    test_register("mlfq_decay_clamps",   test_mlfq_decay_clamps);
    test_register("mlfq_boost_promotes", test_mlfq_boost_promotes);
    test_register("mlfq_dequeue_removes", test_mlfq_dequeue_removes);
    test_register("mlfq_total_count_ok", test_mlfq_total_count_ok);
}
