/**
 * @file mlfq.h
 * @brief Multi-Level Feedback Queue scheduler — priority queues with decay.
 *
 * MLFQ maintains 4 priority levels (0=highest, 3=lowest).  New tasks
 * start at level 0.  When a task exhausts its quantum, it is demoted
 * one level (decay).  A periodic priority boost promotes all tasks to
 * level 0 to prevent starvation.
 *
 * Level  Quanta (ticks)  Use case
 * ----   ---------------  --------
 *   0         2           Interactive / I/O-bound
 *   1         4           Balanced
 *   2         8           CPU-bound
 *   3        16           Background / batch
 *
 * O(1) task selection via a bitmap: bit N set means queue N has tasks.
 * __builtin_ctz finds the lowest set bit (highest priority) in one instruction.
 *
 * References:
 *   Ousterhout, "Scheduling Techniques for Multiprogrammed Systems" (1982)
 *   Tanenbaum & Bos, "Modern Operating Systems" 5th ed. §2.5
 */

#ifndef KERNEL_MLFQ_H
#define KERNEL_MLFQ_H

#include "task.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Number of priority levels. */
#define MLFQ_LEVELS            4

/** Timer ticks between priority boosts (at 100 Hz = 2 seconds). */
#define MLFQ_BOOST_INTERVAL    200

/* =========================================================================
 * Per-level queue
 * ========================================================================= */

typedef struct mlfd_queue {
    task_t *head;       /**< First task in queue. */
    task_t *tail;       /**< Last task in queue. */
    uint32_t count;     /**< Number of tasks in queue. */
} mlfd_queue_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the MLFQ scheduler.
 *
 * Zeros all queues and the bitmap.  Must be called before any
 * mlfq_enqueue().
 */
void mlfq_init(void);

/**
 * @brief Enqueue a task at the specified priority level.
 *
 * Appends the task to the tail of the given queue.  Updates the bitmap
 * if the queue was previously empty.
 *
 * @param task   Task to enqueue (must not already be in a queue).
 * @param level  Priority level (0-3).
 */
void mlfq_enqueue(task_t *task, int level);

/**
 * @brief Remove a task from its current queue.
 *
 * Searches the queue at task->mlfq_level and removes the task.
 * Updates the bitmap if the queue becomes empty.
 *
 * @param task  Task to dequeue (must be in some queue).
 */
void mlfq_dequeue(task_t *task);

/**
 * @brief Pick the highest-priority ready task.
 *
 * Uses the bitmap for O(1) selection.  Does NOT remove the task
 * from its queue — the caller must call mlfq_dequeue() separately.
 *
 * @return Pointer to the highest-priority task, or NULL if no tasks.
 */
task_t *mlfq_pick_highest(void);

/**
 * @brief Demote a task one priority level (decay).
 *
 * Moves the task from its current level to the next lower level.
 * Tasks at the lowest level (3) stay at level 3.
 * Reloads the quantum for the new level.
 *
 * @param task  Task to decay.
 */
void mlfq_decay(task_t *task);

/**
 * @brief Promote all tasks to level 0 (priority boost).
 *
 * Called periodically to prevent starvation.  Moves every task
 * in levels 1-3 to level 0 and reloads their quanta.
 */
void mlfq_boost_all(void);

/**
 * @brief Get the total number of tasks across all queues.
 *
 * @return Sum of counts across all 4 queues.
 */
uint32_t mlfq_total_count(void);

/**
 * @brief Called on every timer tick to drive the priority boost.
 *
 * Increments the boost counter and triggers mlfq_boost_all()
 * when MLFQ_BOOST_INTERVAL ticks have elapsed.
 */
void mlfq_tick(void);

/**
 * @brief Set the serial device for MLFQ logging.
 *
 * @param serial_dev  Initialized serial device.  May be NULL.
 */
void mlfq_log_init(void *serial_dev);

/**
 * @brief Dump MLFQ state to serial (debug).
 */
void mlfq_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_MLFQ_H */
