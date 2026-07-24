/**
 * @file test_usermode.c
 * @brief User-mode task creation and setup tests.
 *
 * Tests registered (10):
 *   um_user_task_create       — task_create(name, entry, false) works
 *   um_user_has_own_as        — user task gets its own address space
 *   um_user_as_not_kernel     — user address space is not the kernel one
 *   um_user_stack_base        — user stack base is TASK_USER_STACK_BASE
 *   um_user_stack_size        — user stack size is TASK_USER_STACK_SIZE
 *   um_user_not_kernel        — is_kernel flag is false
 *   um_user_pcb_aligned       — task struct is 16-byte aligned
 *   um_user_fpu_allocated     — fpu_state buffer is allocated
 *   um_user_kernel_stack      — kernel stack is allocated
 *   um_user_iretq_trampoline  — trampoline address is non-NULL
 */

#include "test.h"
#include "../kernel/task.h"
#include "../kernel/scheduler.h"
#include "../kernel/vmm.h"
#include "../kernel/page_table.h"
#include <stdint.h>
#include <stdbool.h>

/* User task entry that does nothing (would need ring-3 to actually run). */
static void noop_user_entry(void) {
    while (1) { asm volatile ("hlt"); }
}

/* ---- Test: user task creation succeeds ---- */
static void test_um_user_task_create(serial_dev_t *dev) {
    task_t *t = task_create("utest1", noop_user_entry, false);
    ASSERT_NOT_NULL(dev, t);
    task_destroy(t);
}

/* ---- Test: user task has its own address space ---- */
static void test_um_user_has_own_as(serial_dev_t *dev) {
    task_t *t = task_create("utest2", noop_user_entry, false);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_NOT_NULL(dev, t->address_space);
    /* Must not share with kernel address space. */
    address_space_t *kspace = vmm_get_kernel_address_space();
    ASSERT_TRUE(dev, t->address_space != kspace);
    task_destroy(t);
}

/* ---- Test: user address space is not the kernel one ---- */
static void test_um_user_as_not_kernel(serial_dev_t *dev) {
    task_t *t = task_create("utest3", noop_user_entry, false);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_TRUE(dev, t->address_space->is_kernel == false);
    task_destroy(t);
}

/* ---- Test: user stack base is correct ---- */
static void test_um_user_stack_base(serial_dev_t *dev) {
    task_t *t = task_create("utest4", noop_user_entry, false);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_EQ(dev, t->user_stack_base, TASK_USER_STACK_BASE);
    task_destroy(t);
}

/* ---- Test: user stack size is correct ---- */
static void test_um_user_stack_size(serial_dev_t *dev) {
    task_t *t = task_create("utest5", noop_user_entry, false);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_EQ(dev, t->user_stack_size, TASK_USER_STACK_SIZE);
    task_destroy(t);
}

/* ---- Test: is_kernel flag is false ---- */
static void test_um_user_not_kernel(serial_dev_t *dev) {
    task_t *t = task_create("utest6", noop_user_entry, false);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_TRUE(dev, t->is_kernel == false);
    task_destroy(t);
}

/* ---- Test: task struct is 16-byte aligned ---- */
static void test_um_user_pcb_aligned(serial_dev_t *dev) {
    task_t *t = task_create("utest7", noop_user_entry, false);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_TRUE(dev, ((uint64_t)t & 0xF) == 0);
    task_destroy(t);
}

/* ---- Test: FPU state buffer is allocated ---- */
static void test_um_user_fpu_allocated(serial_dev_t *dev) {
    task_t *t = task_create("utest8", noop_user_entry, false);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_NOT_NULL(dev, t->fpu_state);
    task_destroy(t);
}

/* ---- Test: kernel stack is allocated ---- */
static void test_um_user_kernel_stack(serial_dev_t *dev) {
    task_t *t = task_create("utest9", noop_user_entry, false);
    ASSERT_NOT_NULL(dev, t);
    ASSERT_TRUE(dev, t->kernel_stack_base != 0);
    ASSERT_TRUE(dev, t->kernel_stack_size >= 4096);
    task_destroy(t);
}

/* ---- Test: iretq trampoline address is valid ---- */
extern void user_task_iretq_trampoline(void);
static void test_um_user_iretq_trampoline(serial_dev_t *dev) {
    ASSERT_NOT_NULL(dev, (void *)user_task_iretq_trampoline);
}

/* ---- Registration ---- */
void test_register_usermode(void) {
    test_register("um_user_task_create", test_um_user_task_create);
    test_register("um_user_has_own_as", test_um_user_has_own_as);
    test_register("um_user_as_not_kernel", test_um_user_as_not_kernel);
    test_register("um_user_stack_base", test_um_user_stack_base);
    test_register("um_user_stack_size", test_um_user_stack_size);
    test_register("um_user_not_kernel", test_um_user_not_kernel);
    test_register("um_user_pcb_aligned", test_um_user_pcb_aligned);
    test_register("um_user_fpu_allocated", test_um_user_fpu_allocated);
    test_register("um_user_kernel_stack", test_um_user_kernel_stack);
    test_register("um_user_iretq_trampoline", test_um_user_iretq_trampoline);
}
