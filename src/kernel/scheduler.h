/**
 * @file scheduler.h
 * @brief Round-robin preemptive scheduler interface.
 *
 * The scheduler maintains a circular ready queue of tasks. On each
 * timer tick, scheduler_tick() decrements the current task's remaining
 * quantum. When it reaches zero, the scheduler picks the next ready
 * task and calls context_switch() to transfer control.
 *
 * Init order:
 *   task_init() → scheduler_init() → [pit_init, timer_init] → sti
 *
 * After sti, IRQ 0 fires at the PIT frequency, driving scheduler_tick()
 * on every interrupt.
 */

#ifndef KERNEL_SCHEDULER_H
#define KERNEL_SCHEDULER_H

#include "task.h"
#include "isr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the scheduler.
 *
 * Creates the idle task, sets it as current, and marks the scheduler
 * as ready.  Must be called after task_init().
 */
void scheduler_init(void *serial_dev);

/**
 * @brief Add a task to the ready queue.
 *
 * Sets the task state to READY and appends it to the circular
 * ready queue.
 *
 * @param task  Task to add (must be in CREATED or BLOCKED state).
 */
void scheduler_add_task(task_t *task);

/**
 * @brief Remove a task from the ready queue.
 *
 * Removes the task from whatever queue it is in and marks it as ZOMBIE.
 * Does not free resources — call task_destroy() afterwards.
 *
 * @param task  Task to remove.
 */
void scheduler_remove_task(task_t *task);

/**
 * @brief Timer tick callback — drives preemption.
 *
 * Called from timer_irq_handler() on every IRQ 0.  Decrements the
 * current task's remaining quantum.  When it hits zero, selects the
 * next ready task and performs a context switch.
 *
 * This function overrides the weak symbol in timer.c.
 *
 * @param frame  Interrupt frame (unused for round-robin scheduling).
 */
void scheduler_tick(interrupt_frame_t *frame);

/**
 * @brief Get the currently running task.
 *
 * @return Pointer to the current task_t, or NULL if no task is running.
 */
task_t *scheduler_get_current(void);

/**
 * @brief Get the number of tasks in the ready queue.
 *
 * @return Count of tasks in the ready queue (excluding the idle task).
 */
uint64_t scheduler_get_task_count(void);

/**
 * @brief The idle task entry point.
 *
 * This is the code the idle task runs — a simple HLT loop that
 * saves power when no other tasks are ready.  Declared extern
 * so task_create_idle() can reference it.
 */
void idle_task_entry(void);

/**
 * @brief Unblock a sleeping task and re-enqueue it in the scheduler.
 *
 * Transitions the task from BLOCKED to READY and adds it to the
 * MLFQ ready queue at level 0.
 *
 * @param task  Task to wake (must be in BLOCKED state).
 */
void scheduler_wake_task(task_t *task);

/**
 * @brief Remove a task from the scheduler without changing its state.
 *
 * Used by task_wait to block a task: removes it from the MLFQ
 * ready queue so it is no longer scheduled.
 *
 * @param task  Task to remove from the ready queue.
 */
void scheduler_unqueue_task(task_t *task);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_SCHEDULER_H */
