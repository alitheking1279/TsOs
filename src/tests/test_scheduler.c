/**
 * @file test_scheduler.c
 * @brief Scheduler tests — init, add/remove, tick, quantum, idle task.
 *
 * IMPORTANT: scheduler_tick() calls context_switch() when the quantum
 * expires. context_switch() never returns to the caller. The switched-to
 * task must switch back for the test to continue.
 *
 * The sched_roundtrip_task uses a captured g_self pointer (set before
 * the switch) because context_switch does not update task_get_current().
 *
 * Tests registered (6):
 *   sched_init_done        — scheduler_get_current() returns non-NULL after init
 *   sched_idle_task_exists — idle task (PID 0) is the initial current task
 *   sched_add_task         — adding a task increments task count
 *   sched_remove_task      — removing a task decrements task count
 *   sched_tick_no_preempt  — tick before quantum expires doesn't switch
 *   sched_quantum_expiry   — tick at quantum=0 triggers context switch
 */

#include "test.h"
#include "../kernel/task.h"
#include "../kernel/scheduler.h"
#include "../kernel/isr.h"
#include <stdint.h>

extern void context_switch(task_t *prev, task_t *next);

/* Home task for round-trip context switches. */
static task_t *g_home_task = NULL;

/* The task that is executing the entry function (set before switch). */
static task_t *g_self = NULL;

/* Flag set by test tasks to confirm they ran. */
static volatile int g_test_task_ran = 0;

/**
 * @brief Test task entry — sets flag and switches back to home.
 *
 * Uses g_self (captured before the switch) instead of task_get_current()
 * because context_switch does not update the global current-task pointer.
 */
static void sched_roundtrip_task(void) {
    g_test_task_ran = 1;
    context_switch(g_self, g_home_task);
    while (1) { asm volatile ("hlt"); }
}

/* ---- Test: scheduler is initialized ---- */
static void test_sched_init_done(serial_dev_t *dev) {
    task_t *cur = scheduler_get_current();
    ASSERT_NOT_NULL(dev, cur);
}

/* ---- Test: idle task is current ---- */
static void test_sched_idle_task_exists(serial_dev_t *dev) {
    task_t *cur = scheduler_get_current();
    ASSERT_NOT_NULL(dev, cur);
    ASSERT_EQ(dev, cur->pid, (uint64_t)0);
    ASSERT_TRUE(dev, cur->is_kernel);
}

/* ---- Test: adding a task increments count ---- */
static void test_sched_add_task(serial_dev_t *dev) {
    uint64_t before = scheduler_get_task_count();
    task_t *t = task_create("sched_test", sched_roundtrip_task, true);
    ASSERT_NOT_NULL(dev, t);
    scheduler_add_task(t);
    ASSERT_EQ(dev, scheduler_get_task_count(), before + 1);
    scheduler_remove_task(t);
    task_destroy(t);
}

/* ---- Test: removing a task decrements count ---- */
static void test_sched_remove_task(serial_dev_t *dev) {
    task_t *t = task_create("sched_rm", sched_roundtrip_task, true);
    ASSERT_NOT_NULL(dev, t);
    scheduler_add_task(t);
    uint64_t count_after_add = scheduler_get_task_count();
    scheduler_remove_task(t);
    ASSERT_EQ(dev, scheduler_get_task_count(), count_after_add - 1);
    task_destroy(t);
}

/* ---- Test: tick before quantum expires doesn't switch ---- */
static void test_sched_tick_no_preempt(serial_dev_t *dev) {
    task_t *before = scheduler_get_current();
    ASSERT_NOT_NULL(dev, before);

    /* The current task (idle) has remaining_ticks > 0.
     * A single tick should not trigger a context switch. */
    interrupt_frame_t dummy = {0};
    scheduler_tick(&dummy);

    task_t *after = scheduler_get_current();
    ASSERT_EQ(dev, (uint64_t)before, (uint64_t)after);
}

/* ---- Test: quantum expiry triggers a switch ---- */
static void test_sched_quantum_expiry(serial_dev_t *dev) {
    /* Create a real task and add it to the ready queue. */
    task_t *t = task_create("sched_qe", sched_roundtrip_task, true);
    ASSERT_NOT_NULL(dev, t);
    scheduler_add_task(t);

    g_test_task_ran = 0;
    g_self = t;
    g_home_task = scheduler_get_current();

    /* Force the idle task's remaining_ticks to 0. */
    task_t *idle = scheduler_get_current();
    idle->remaining_ticks = 0;

    interrupt_frame_t dummy = {0};
    scheduler_tick(&dummy);

    /* After the tick + context switch round-trip, we're back.
     * The test task should have executed and switched back. */
    ASSERT_EQ(dev, g_test_task_ran, (uint64_t)1);

    /* The current task should be back to idle (home). */
    task_t *after = scheduler_get_current();
    ASSERT_EQ(dev, (uint64_t)after, (uint64_t)idle);

    /* Clean up. */
    scheduler_remove_task(t);
    task_destroy(t);
}

/* ---- Registration ---- */
void test_register_scheduler(void) {
    test_register("sched_init_done", test_sched_init_done);
    test_register("sched_idle_task_exists", test_sched_idle_task_exists);
    test_register("sched_add_task", test_sched_add_task);
    test_register("sched_remove_task", test_sched_remove_task);
    test_register("sched_tick_no_preempt", test_sched_tick_no_preempt);
    test_register("sched_quantum_expiry", test_sched_quantum_expiry);
}
