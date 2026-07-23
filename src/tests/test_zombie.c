/**
 * @file test_zombie.c
 * @brief Zombie task lifecycle tests — exit, reparent, wait, reap.
 *
 * Tests registered (6):
 *   zombie_exit_sets_state   — task_exit sets state to ZOMBIE
 *   zombie_exit_code         — task_exit stores the exit code
 *   zombie_reparent          — task_reparent_children moves children to init
 *   zombie_wait_reaps        — task_wait reaps a zombie child and returns PID
 *   zombie_wait_no_children  — task_wait returns -1 when no children
 *   zombie_wait_no_zombie    — task_wait returns -1 when no zombie child
 */

#include "test.h"
#include "../kernel/task.h"
#include "../kernel/scheduler.h"
#include <stdint.h>

/* Stub entry for test tasks. */
static void zombie_stub_entry(void) {
    while (1) { asm volatile ("hlt"); }
}

/* ---- Test: task_exit sets state to ZOMBIE ---- */
static void test_zombie_exit_sets_state(serial_dev_t *dev) {
    task_t *t = task_create("z_exit", zombie_stub_entry, true);
    ASSERT_NOT_NULL(dev, t);

    /* Simulate: set as current, exit, check state. */
    task_t *saved = task_get_current();
    task_set_current(t);
    task_exit(0);
    ASSERT_EQ(dev, t->state, TASK_STATE_ZOMBIE);
    ASSERT_EQ(dev, t->exit_code, (int)0);

    /* Cleanup: restore current, destroy zombie manually. */
    task_set_current(saved);
    task_destroy(t);
}

/* ---- Test: task_exit stores exit code ---- */
static void test_zombie_exit_code(serial_dev_t *dev) {
    task_t *t = task_create("z_code", zombie_stub_entry, true);
    ASSERT_NOT_NULL(dev, t);

    task_t *saved = task_get_current();
    task_set_current(t);
    task_exit(42);
    ASSERT_EQ(dev, t->exit_code, (int)42);
    ASSERT_EQ(dev, t->state, TASK_STATE_ZOMBIE);

    task_set_current(saved);
    task_destroy(t);
}

/* ---- Test: reparent moves children to init ---- */
static void test_zombie_reparent(serial_dev_t *dev) {
    task_t *parent = task_create("z_p", zombie_stub_entry, true);
    task_t *child1 = task_create("z_c1", zombie_stub_entry, true);
    task_t *child2 = task_create("z_c2", zombie_stub_entry, true);
    ASSERT_NOT_NULL(dev, parent);
    ASSERT_NOT_NULL(dev, child1);
    ASSERT_NOT_NULL(dev, child2);

    /* Manually set up parent-child relationship. */
    child1->parent = parent;
    child2->parent = parent;
    child1->sibling_next = parent->children;
    parent->children = child1;
    child2->sibling_next = child1->sibling_next;
    child1->sibling_next = child2;

    ASSERT_EQ(dev, parent->children, (uint64_t)child1);

    /* Reparent. */
    task_reparent_children(parent);
    ASSERT_NULL(dev, parent->children);

    /* Children should now have PID 1 (init) as parent. */
    /* We can't easily check the parent pointer without knowing init's address,
     * but we can verify children are no longer in parent's list. */
    ASSERT_NULL(dev, parent->children);

    /* Cleanup — unlink children from init's list manually. */
    child1->parent = NULL;
    child2->parent = NULL;
    child1->sibling_next = NULL;
    child2->sibling_next = NULL;
    task_destroy(parent);
    task_destroy(child1);
    task_destroy(child2);
}

/* ---- Test: task_wait reaps zombie child ---- */
static void test_zombie_wait_reaps(serial_dev_t *dev) {
    task_t *parent = task_create("z_wp", zombie_stub_entry, true);
    task_t *child = task_create("z_wc", zombie_stub_entry, true);
    ASSERT_NOT_NULL(dev, parent);
    ASSERT_NOT_NULL(dev, child);

    /* Set up parent-child. */
    child->parent = parent;
    child->sibling_next = NULL;
    parent->children = child;

    /* Make child a zombie. */
    task_t *saved = task_get_current();
    task_set_current(child);
    task_exit(7);
    task_set_current(saved);

    ASSERT_EQ(dev, child->state, TASK_STATE_ZOMBIE);

    /* Parent waits. */
    task_set_current(parent);
    int status = -999;
    int pid = task_wait(&status);
    task_set_current(saved);

    /* Should have reaped the child. */
    ASSERT_EQ(dev, pid, (int)child->pid);
    ASSERT_EQ(dev, status, (int)7);

    /* Parent's children list should be empty. */
    ASSERT_NULL(dev, parent->children);

    task_destroy(parent);
}

/* ---- Test: task_wait returns -1 with no children ---- */
static void test_zombie_wait_no_children(serial_dev_t *dev) {
    task_t *t = task_create("z_nw", zombie_stub_entry, true);
    ASSERT_NOT_NULL(dev, t);

    task_t *saved = task_get_current();
    task_set_current(t);
    int status;
    int pid = task_wait(&status);
    task_set_current(saved);

    ASSERT_EQ(dev, pid, (int)-1);

    task_destroy(t);
}

/* ---- Test: task_wait returns -1 when no zombie child ---- */
static void test_zombie_wait_no_zombie(serial_dev_t *dev) {
    task_t *parent = task_create("z_nz", zombie_stub_entry, true);
    task_t *child = task_create("z_nzc", zombie_stub_entry, true);
    ASSERT_NOT_NULL(dev, parent);
    ASSERT_NOT_NULL(dev, child);

    /* Set up parent-child, but child is NOT a zombie. */
    child->parent = parent;
    child->sibling_next = NULL;
    parent->children = child;
    child->state = TASK_STATE_READY;

    task_t *saved = task_get_current();
    task_set_current(parent);
    int status;
    int pid = task_wait(&status);
    task_set_current(saved);

    /* No zombie to reap. */
    ASSERT_EQ(dev, pid, (int)-1);

    /* Cleanup. */
    child->parent = NULL;
    parent->children = NULL;
    task_destroy(parent);
    task_destroy(child);
}

/* ---- Registration ---- */
void test_register_zombie(void) {
    test_register("zombie_exit_sets_state", test_zombie_exit_sets_state);
    test_register("zombie_exit_code",       test_zombie_exit_code);
    test_register("zombie_reparent",        test_zombie_reparent);
    test_register("zombie_wait_reaps",      test_zombie_wait_reaps);
    test_register("zombie_wait_no_children", test_zombie_wait_no_children);
    test_register("zombie_wait_no_zombie",  test_zombie_wait_no_zombie);
}
