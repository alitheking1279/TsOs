/**
 * @file test_task.c
 * @brief Task management tests — creation, PID, state, destruction.
 *
 * Tests registered (8):
 *   task_create_basic        — task_create returns non-NULL
 *   task_pid_unique          — two tasks get different PIDs
 *   task_pid_sequential      — PIDs are sequential (1, 2, 3...)
 *   task_state_created       — new task starts in CREATED state
 *   task_state_name          — task_state_char returns correct chars
 *   task_kernel_stack        — kernel stack is allocated and non-NULL
 *   task_address_space       — kernel task shares kernel address space
 *   task_destroy_cleans_up   — destroyed task's memory is freed (no crash on re-test)
 */

#include "test.h"
#include "../kernel/task.h"
#include "../kernel/scheduler.h"
#include "../kernel/vmm.h"
#include <stdint.h>

/* Simple task entry that does nothing. */
static void noop_task(void) {
    while (1) { asm volatile ("hlt"); }
}

/* ---- Test: basic creation ---- */
static void test_task_create_basic(serial_dev_t *dev) {
    task_t *t = task_create("test_a", noop_task, true);
    ASSERT_NOT_NULL(dev, t);
    /* Clean up. */
    task_destroy(t);
}

/* ---- Test: unique PIDs ---- */
static void test_task_pid_unique(serial_dev_t *dev) {
    task_t *a = task_create("test_b", noop_task, true);
    task_t *c = task_create("test_c", noop_task, true);
    ASSERT_NOT_NULL(dev, a);
    ASSERT_NOT_NULL(dev, c);
    ASSERT_TRUE(dev, a->pid != c->pid);
    task_destroy(a);
    task_destroy(c);
}

/* ---- Test: sequential PIDs ---- */
static void test_task_pid_sequential(serial_dev_t *dev) {
    uint64_t before = task_get_next_pid();
    task_t *t = task_create("test_d", noop_task, true);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_EQ(dev, t->pid, before);
    ASSERT_EQ(dev, task_get_next_pid(), before + 1);
    task_destroy(t);
}

/* ---- Test: state is CREATED ---- */
static void test_task_state_created(serial_dev_t *dev) {
    task_t *t = task_create("test_e", noop_task, true);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_EQ(dev, t->state, TASK_STATE_CREATED);
    task_destroy(t);
}

/* ---- Test: state char conversion ---- */
static void test_task_state_name(serial_dev_t *dev) {
    ASSERT_EQ(dev, task_state_char(TASK_STATE_CREATED), 'C');
    ASSERT_EQ(dev, task_state_char(TASK_STATE_READY), 'R');
    ASSERT_EQ(dev, task_state_char(TASK_STATE_RUNNING), 'X');
    ASSERT_EQ(dev, task_state_char(TASK_STATE_BLOCKED), 'B');
    ASSERT_EQ(dev, task_state_char(TASK_STATE_SLEEPING), 'S');
    ASSERT_EQ(dev, task_state_char(TASK_STATE_ZOMBIE), 'Z');
    ASSERT_EQ(dev, task_state_char(TASK_STATE_DEAD), 'D');
}

/* ---- Test: kernel stack is allocated ---- */
static void test_task_kernel_stack(serial_dev_t *dev) {
    task_t *t = task_create("test_f", noop_task, true);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_TRUE(dev, t->kernel_stack_base != 0);
    ASSERT_TRUE(dev, t->kernel_stack_size >= 4096);
    task_destroy(t);
}

/* ---- Test: kernel task shares kernel address space ---- */
static void test_task_address_space(serial_dev_t *dev) {
    task_t *t = task_create("test_g", noop_task, true);
    ASSERT_NOT_NULL(dev, t);
    address_space_t *kspace = vmm_get_kernel_address_space();
    ASSERT_TRUE(dev, t->address_space == kspace);
    task_destroy(t);
}

/* ---- Test: destroy doesn't crash and memory is reusable ---- */
static void test_task_destroy_cleans_up(serial_dev_t *dev) {
    task_t *a = task_create("test_h", noop_task, true);
    ASSERT_NOT_NULL(dev, a);
    task_destroy(a);
    /* Creating another task should succeed — the memory was freed. */
    task_t *b = task_create("test_i", noop_task, true);
    ASSERT_NOT_NULL(dev, b);
    task_destroy(b);
}

/* ---- Registration ---- */
void test_register_task(void) {
    test_register("task_create_basic", test_task_create_basic);
    test_register("task_pid_unique", test_task_pid_unique);
    test_register("task_pid_sequential", test_task_pid_sequential);
    test_register("task_state_created", test_task_state_created);
    test_register("task_state_name", test_task_state_name);
    test_register("task_kernel_stack", test_task_kernel_stack);
    test_register("task_address_space", test_task_address_space);
    test_register("task_destroy_cleans_up", test_task_destroy_cleans_up);
}
