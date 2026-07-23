/**
 * @file scheduler.c
 * @brief MLFQ preemptive scheduler implementation.
 *
 * Multi-Level Feedback Queue with 4 priority levels.  New tasks start
 * at level 0 (highest priority, shortest quantum).  On quantum expiry,
 * tasks are demoted one level (decay).  A periodic boost promotes all
 * tasks to level 0 every 200 ticks to prevent starvation.
 *
 * The idle task (PID 0) is enqueued at level 3 and only runs when no
 * other tasks are ready.
 */

#include "scheduler.h"
#include "mlfq.h"
#include "task.h"
#include "slab.h"
#include "gdt.h"
#include "vmm.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Static state
 * ========================================================================= */

/** Currently executing task. */
static task_t *g_current = NULL;

/** The idle task (always kept at level 3 as a fallback). */
static task_t *g_idle_task = NULL;

/** Number of non-idle tasks across all MLFQ queues. */
static uint64_t g_ready_count = 0;

/** Serial device for logging. */
static serial_dev_t *g_sched_serial = NULL;

/** True after scheduler_init() completes. */
static bool g_sched_initialized = false;

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

extern void context_switch(task_t *prev, task_t *next);

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void sched_log(const char *msg) {
    if (g_sched_serial) {
        serial_write_string(g_sched_serial, msg);
    }
}

static void sched_log_hex(uint64_t val) {
    if (!g_sched_serial) return;
    static const char hex[] = "0123456789ABCDEF";
    serial_write_string(g_sched_serial, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_write_char(g_sched_serial, hex[(val >> i) & 0xF]);
    }
}

static void sched_log_u64(uint64_t val) {
    if (!g_sched_serial) return;
    if (val == 0) { serial_write_char(g_sched_serial, '0'); return; }
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    while (val > 0) { buf[--i] = (char)('0' + (val % 10)); val /= 10; }
    serial_write_string(g_sched_serial, &buf[i]);
}

/* =========================================================================
 * Idle task
 * ========================================================================= */

void idle_task_entry(void) {
    sched_log("[INFO] Idle task running.\r\n");
    while (1) {
        asm volatile ("hlt");
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void scheduler_init(void *serial_dev) {
    g_sched_serial = (serial_dev_t *)serial_dev;

    sched_log("[INFO] Scheduler initializing (MLFQ)...\r\n");

    /* Initialize MLFQ queues. */
    mlfq_init();
    mlfq_log_init(serial_dev);

    /* Create the idle task. */
    g_idle_task = task_create_idle();
    if (!g_idle_task) {
        sched_log("[ERR] Failed to create idle task!\r\n");
        while (1) { asm volatile ("cli; hlt"); }
    }

    /* Enqueue idle at level 3 (lowest priority — runs only when nothing else is ready). */
    mlfq_enqueue(g_idle_task, 3);

    /* Set the idle task as current (before sti, no real task is running). */
    g_current = g_idle_task;
    g_current->state = TASK_STATE_RUNNING;
    task_set_current(g_current);

    g_ready_count = 0;  /* Don't count idle in the ready count. */
    g_sched_initialized = true;

    sched_log("[INFO] Scheduler initialized (MLFQ, 4 levels).\r\n");
}

void scheduler_add_task(task_t *task) {
    if (!task) return;

    sched_log("[SCHED] Adding task PID=");
    sched_log_hex(task->pid);
    sched_log(" name=");
    sched_log(task->name);
    sched_log("\r\n");

    /* New tasks start at level 0 (highest priority). */
    mlfq_enqueue(task, 0);
    g_ready_count++;
}

void scheduler_remove_task(task_t *task) {
    if (!task) return;

    sched_log("[SCHED] Removing task PID=");
    sched_log_hex(task->pid);
    sched_log("\r\n");

    mlfq_dequeue(task);
    task->state = TASK_STATE_ZOMBIE;

    if (task != g_idle_task) {
        g_ready_count--;
    }
}

void scheduler_tick(interrupt_frame_t *frame) {
    (void)frame;

    /* Nothing to do if scheduler isn't ready or only idle task exists. */
    if (!g_sched_initialized || !g_current) return;

    /* Drive MLFQ periodic boost. */
    mlfq_tick();

    /* Decrement the current task's quantum. */
    if (g_current->remaining_ticks > 0) {
        g_current->remaining_ticks--;
    }

    /* If the quantum hasn't expired, keep running the current task. */
    if (g_current->remaining_ticks > 0) return;

    /* Quantum expired — pick the highest-priority ready task. */
    task_t *prev = g_current;

    /* Dequeue prev from its current level (it exhausted its quantum). */
    if (prev->state == TASK_STATE_RUNNING) {
        mlfq_decay(prev);
    }

    /* Pick the next highest-priority task. */
    task_t *next = mlfq_pick_highest();
    if (!next) {
        next = g_idle_task;
    }

    /* If the next task is the same as the current, no switch needed. */
    if (next == prev) return;

    /* Dequeue next from its queue (we're about to run it). */
    mlfq_dequeue(next);
    next->state = TASK_STATE_RUNNING;

    /* Update current task pointers. */
    g_current = next;
    task_set_current(next);

    /* Update TSS.RSP0 so ring-3 → ring-0 transitions use the right kernel stack. */
    tss_set_rsp0(next->kernel_stack_base + next->kernel_stack_size);

    /* --- Pre-context-switch: FPU save + CR3 switch --- */
    /* Save previous task's FPU state (kernel address space is active, safe to access). */
    if (prev->fpu_state) {
        __asm__ volatile ("fxsave (%0)" : : "r" (prev->fpu_state) : "memory");
    }

    /* Switch address space if next task has a different one.
     * Kernel tasks share the kernel address space, so no switch needed.
     * Must happen BEFORE restoring next's FPU state so the buffer is accessible. */
    if (next->address_space != prev->address_space) {
        vmm_switch_address_space(next->address_space);
    }

    /* Restore next task's FPU state (kernel half is mapped in all address spaces). */
    if (next->fpu_state) {
        __asm__ volatile ("fxrstor (%0)" : : "r" (next->fpu_state) : "memory");
    }

    /* Perform the context switch! */
    context_switch(prev, next);

    /* We are back on prev.  Restore the current-task pointer. */
    g_current = prev;
    task_set_current(prev);
}

task_t *scheduler_get_current(void) {
    return g_current;
}

uint64_t scheduler_get_task_count(void) {
    return g_ready_count;
}
