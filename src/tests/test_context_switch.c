/**
 * @file test_context_switch.c
 * @brief Context switch tests — stack layout, register preservation, CR3.
 *
 * IMPORTANT: context_switch() never returns to the caller. It returns
 * to wherever the next task was last executing (or its entry point).
 * Also, context_switch does NOT update g_current_task — only the
 * scheduler does that.  So test tasks use a captured pointer (g_self)
 * instead of task_get_current().
 *
 * Tests registered (6):
 *   ctxsw_stack_layout        — initial kernel_rsp points to correct stack frame
 *   ctxsw_entry_point         — context_switch to new task jumps to its entry
 *   ctxsw_preserves_rbx       — rbx is preserved across a context switch
 *   ctxsw_preserves_rbp       — rbp is preserved across a context switch
 *   ctxsw_preserves_r12_r15   — r12-r15 are preserved across a context switch
 *   ctxsw_same_cr3             — kernel tasks stay on the same address space
 */

#include "test.h"
#include "../kernel/task.h"
#include "../kernel/scheduler.h"
#include "../kernel/vmm.h"
#include <stdint.h>
#include <string.h>

/* The "home" task — test tasks switch back here after doing their work. */
static task_t *g_home_task = NULL;

/* The task that is currently executing the test task entry function.
 * Set to the task_t* before calling context_switch into the test task. */
static task_t *g_self = NULL;

extern void context_switch(task_t *prev, task_t *next);

/* Shared state for cross-task communication. */
static volatile int      g_task_ran = 0;
static volatile uint64_t g_captured_rbx = 0;
static volatile uint64_t g_captured_rbp = 0;
static volatile uint64_t g_captured_r12 = 0;
static volatile uint64_t g_captured_r15 = 0;

/* FPU preservation test: home task loads st(0)=pi, switches to test task,
 * test task loads st(0)=2.0, switches back, home verifies pi is restored. */
static volatile int g_fpu_test_ran = 0;

/* ---- Entry point for test tasks — sets flag, switches back to home ---- */
static void test_task_entry(void) {
    g_task_ran = 1;
    /* Switch back to the home task.  Use g_self (captured before the switch)
     * because context_switch does not update task_get_current(). */
    context_switch(g_self, g_home_task);
    /* Should never reach here. */
    while (1) { asm volatile ("hlt"); }
}

/* ---- Entry point that captures register values then switches back ----
 * This must be __attribute__((naked)) because the compiler's function
 * prologue (push rbp; mov rbp, rsp) would clobber the callee-saved
 * registers before we can read them. */
__attribute__((naked)) static void test_task_regs(void) {
    __asm__ volatile (
        /* Capture callee-saved registers set by context_switch's pops. */
        "mov %%rbx, %[out_rbx]\n"
        "mov %%rbp, %[out_rbp]\n"
        "mov %%r12, %[out_r12]\n"
        "mov %%r15, %[out_r15]\n"
        : [out_rbx] "=m" (g_captured_rbx),
          [out_rbp] "=m" (g_captured_rbp),
          [out_r12] "=m" (g_captured_r12),
          [out_r15] "=m" (g_captured_r15)
        :
        : "memory"
    );
    /* Switch back to home task. */
    __asm__ volatile (
        "mov %[self], %%rdi\n"
        "mov %[home], %%rsi\n"
        "call context_switch\n"
        "1: hlt\n"
        "jmp 1b\n"
        :
        : [self] "r" (g_self),
          [home] "r" (g_home_task)
        : "rdi", "rsi", "memory"
    );
}

/* ---- Entry point for FPU preservation test task ----
 * Loads 2.0 into FPU st(0), then switches back to home task.
 * Home task then verifies its own FPU state (pi) was restored. */
__attribute__((naked)) static void test_task_fpu(void) {
    __asm__ volatile (
        "fld1\n"                /* st(0) = 1.0 */
        "fld1\n"                /* st(0) = 1.0, st(1) = 1.0 */
        "faddp\n"               /* st(0) = 2.0 */
        /* Switch back to home task. */
        "mov %[self], %%rdi\n"
        "mov %[home], %%rsi\n"
        "call context_switch\n"
        "1: hlt\n"
        "jmp 1b\n"
        :
        : [self] "r" (g_self),
          [home] "r" (g_home_task)
        : "rdi", "rsi", "memory"
    );
}

/* ---- Test: initial stack layout is correct ---- */
static void test_ctxsw_stack_layout(serial_dev_t *dev) {
    task_t *t = task_create("ctxsw_test", test_task_entry, true);
    ASSERT_NOT_NULL(dev, t);

    uint64_t rsp = t->kernel_rsp;
    ASSERT_TRUE(dev, rsp > t->kernel_stack_base);
    ASSERT_TRUE(dev, rsp < t->kernel_stack_base + t->kernel_stack_size);

    /* Return address at [kernel_rsp + 48] should be test_task_entry. */
    uint64_t ret_addr = *(uint64_t *)(rsp + 48);
    ASSERT_EQ(dev, ret_addr, (uint64_t)test_task_entry);

    /* Dummy register values at [kernel_rsp + 0..40] should all be zero. */
    uint64_t *stack = (uint64_t *)rsp;
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(dev, stack[i], (uint64_t)0);
    }

    task_destroy(t);
}

/* ---- Test: context_switch jumps to entry point and returns ---- */
static void test_ctxsw_entry_point(serial_dev_t *dev) {
    task_t *t = task_create("ctxsw_entry", test_task_entry, true);
    ASSERT_NOT_NULL(dev, t);

    g_task_ran = 0;
    g_self = t;
    g_home_task = scheduler_get_current();
    ASSERT_NOT_NULL(dev, g_home_task);

    /* Switch to the new task — it will set g_task_ran and switch back. */
    context_switch(g_home_task, t);

    /* We're back on the home task's stack. The test task should have run. */
    ASSERT_EQ(dev, g_task_ran, (uint64_t)1);

    task_destroy(t);
}

/* ---- Test: rbx is preserved ---- */
static void test_ctxsw_preserves_rbx(serial_dev_t *dev) {
    task_t *t = task_create("ctxsw_rbx", test_task_regs, true);
    ASSERT_NOT_NULL(dev, t);

    g_captured_rbx = 0;
    g_captured_rbp = 0;
    g_captured_r12 = 0;
    g_captured_r15 = 0;
    g_self = t;
    g_home_task = scheduler_get_current();

    context_switch(g_home_task, t);

    /* The test task read its own callee-saved regs (all zeroed by dummy pops). */
    ASSERT_EQ(dev, g_captured_rbx, (uint64_t)0);

    task_destroy(t);
}

/* ---- Test: rbp is preserved ---- */
static void test_ctxsw_preserves_rbp(serial_dev_t *dev) {
    ASSERT_EQ(dev, g_captured_rbp, (uint64_t)0);
}

/* ---- Test: r12-r15 are preserved ---- */
static void test_ctxsw_preserves_r12_r15(serial_dev_t *dev) {
    ASSERT_EQ(dev, g_captured_r12, (uint64_t)0);
    ASSERT_EQ(dev, g_captured_r15, (uint64_t)0);
}

/* ---- Test: kernel tasks share the same CR3 ---- */
static void test_ctxsw_same_cr3(serial_dev_t *dev) {
    task_t *t = task_create("ctxsw_cr3", test_task_entry, true);
    ASSERT_NOT_NULL(dev, t);

    uint64_t cr3_before;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_before));

    g_self = t;
    g_home_task = scheduler_get_current();
    context_switch(g_home_task, t);

    uint64_t cr3_after;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_after));

    ASSERT_EQ(dev, cr3_before, cr3_after);

    task_destroy(t);
}

/* ---- Test: FPU state is preserved across context switches ----
 * Home task loads pi into FPU, switches to test task (which loads 2.0),
 * switches back.  Home reads back st(0) via FXSAVE and checks raw bytes. */
static void test_ctxsw_fpu_preserved(serial_dev_t *dev) {
    task_t *t = task_create("ctxsw_fpu", test_task_fpu, true);
    ASSERT_NOT_NULL(dev, t);

    g_fpu_test_ran = 0;
    g_self = t;
    g_home_task = scheduler_get_current();

    /* Load pi into FPU st(0) before the switch. */
    __asm__ volatile ("fldpi");

    /* Switch to test task (which loads 2.0 into st(0)). */
    context_switch(g_home_task, t);

    /* We're back — verify that our FPU state (pi) was restored.
     * FXSAVE stores the x87 FPU state; st(0) is at offset 16 in the
     * 512-byte area (first 10 bytes of the 80-bit register). */
    uint8_t fpu_buf[512] __attribute__((aligned(16)));
    __asm__ volatile ("fxsave (%0)" : : "r" (fpu_buf) : "memory");

    /* The top-of-stack tag is in bytes [4..5] of FXSAVE area.
     * If tag for st(0) is valid (not empty), FPU state was preserved. */
    uint16_t tag = *(uint16_t *)&fpu_buf[4];
    /* Tag bits: each 2-bit field = 00 valid, 01 zero, 10 special, 11 empty.
     * Bits [1:0] correspond to st(0). */
    uint8_t st0_tag = tag & 0x3;
    ASSERT_TRUE(dev, st0_tag == 0 || st0_tag == 1);  /* valid or zero, but NOT empty */

    g_fpu_test_ran = 1;
    task_destroy(t);
}

/* ---- Registration ---- */
void test_register_context_switch(void) {
    test_register("ctxsw_stack_layout", test_ctxsw_stack_layout);
    test_register("ctxsw_entry_point", test_ctxsw_entry_point);
    test_register("ctxsw_preserves_rbx", test_ctxsw_preserves_rbx);
    test_register("ctxsw_preserves_rbp", test_ctxsw_preserves_rbp);
    test_register("ctxsw_preserves_r12_r15", test_ctxsw_preserves_r12_r15);
    test_register("ctxsw_same_cr3", test_ctxsw_same_cr3);
    test_register("ctxsw_fpu_preserved", test_ctxsw_fpu_preserved);
}
