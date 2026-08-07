/**
 * @file mlfq.c
 * @brief Multi-Level Feedback Queue scheduler implementation.
 *
 * Four priority levels with per-level quanta.  Bitmap-based O(1)
 * highest-priority selection.  Decay on quantum expiry, periodic
 * boost to prevent starvation.
 */

#include "mlfq.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Per-level quantum table (in timer ticks at 100 Hz)
 *
 * Level 0: 2 ticks (20 ms) — interactive tasks get fast response
 * Level 1: 4 ticks (40 ms) — balanced
 * Level 2: 8 ticks (80 ms) — CPU-bound
 * Level 3: 16 ticks (160 ms) — background/batch
 * ========================================================================= */

static const uint32_t mlfq_quanta[MLFQ_LEVELS] = { 2, 4, 8, 16 };

/* =========================================================================
 * Global state
 * ========================================================================= */

/** Per-level ready queues. */
static mlfd_queue_t mlfq_queues[MLFQ_LEVELS];

/**
 * Bitmap of non-empty queues.  Bit N set = queue N has at least one task.
 * __builtin_ctz(bitmap) returns the index of the lowest set bit,
 * giving the highest-priority non-empty queue in O(1).
 */
static uint32_t mlfq_bitmap;

/** Ticks since last priority boost. */
static uint64_t g_ticks_since_boost;

/** Serial device for logging. */
static serial_dev_t *g_mlfq_serial = NULL;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void mlfq_log(const char *msg) {
    if (g_mlfq_serial) serial_write_string(g_mlfq_serial, msg);
}

static void mlfq_log_u64(uint64_t val) {
    if (!g_mlfq_serial) return;
    if (val == 0) { serial_write_char(g_mlfq_serial, '0'); return; }
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    while (val > 0) { buf[--i] = (char)('0' + (val % 10)); val /= 10; }
    serial_write_string(g_mlfq_serial, &buf[i]);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void mlfq_init(void) {
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        mlfq_queues[i].head = NULL;
        mlfq_queues[i].tail = NULL;
        mlfq_queues[i].count = 0;
    }
    mlfq_bitmap = 0;
    g_ticks_since_boost = 0;
    g_mlfq_serial = NULL;

    mlfq_log("[MLFQ] Initialized (4 levels, quanta=");
    mlfq_log_u64(mlfq_quanta[0]);
    mlfq_log("/");
    mlfq_log_u64(mlfq_quanta[1]);
    mlfq_log("/");
    mlfq_log_u64(mlfq_quanta[2]);
    mlfq_log("/");
    mlfq_log_u64(mlfq_quanta[3]);
    mlfq_log(")\r\n");
}

void mlfq_log_init(void *serial_dev) {
    g_mlfq_serial = (serial_dev_t *)serial_dev;
}

void mlfq_enqueue(task_t *task, int level) {
    if (!task || level < 0 || level >= MLFQ_LEVELS) return;

    mlfd_queue_t *q = &mlfq_queues[level];

    /* Clear scheduling linkage. */
    task->next = NULL;
    task->mlfq_level = level;
    task->state = TASK_STATE_READY;
    task->remaining_ticks = mlfq_quanta[level];
    task->time_slice = mlfq_quanta[level];

    /* Append to tail. */
    if (q->tail) {
        q->tail->next = task;
    } else {
        q->head = task;
    }
    q->tail = task;
    q->count++;

    /* Update bitmap. */
    mlfq_bitmap |= (1u << level);
}

void mlfq_dequeue(task_t *task) {
    if (!task) return;

    int level = task->mlfq_level;
    if (level < 0 || level >= MLFQ_LEVELS) return;

    mlfd_queue_t *q = &mlfq_queues[level];

    /* Walk the list to find and remove the task. */
    task_t **pp = &q->head;
    while (*pp) {
        if (*pp == task) {
            *pp = task->next;
            if (q->head == NULL) {
                q->tail = NULL;
                mlfq_bitmap &= ~(1u << level);
            } else if (q->tail == task) {
                /* Find new tail. */
                task_t *t = q->head;
                while (t->next) t = t->next;
                q->tail = t;
            }
            q->count--;
            task->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

task_t *mlfq_pick_highest(void) {
    if (mlfq_bitmap == 0) return NULL;

    int level = __builtin_ctz(mlfq_bitmap);
    return mlfq_queues[level].head;
}

void mlfq_decay(task_t *task) {
    if (!task) return;

    int current_level = task->mlfq_level;
    if (current_level >= MLFQ_LEVELS - 1) {
        /* Already at lowest level — reload quantum and rotate to the tail
         * of the L3 queue.  scheduler_tick() dequeues a task when it is
         * picked to run, so without re-enqueueing here the task would be
         * lost from every queue on its next L3 quantum expiry (orphan). */
        mlfq_dequeue(task);
        mlfq_enqueue(task, current_level);
        return;
    }

    /* Remove from current queue. */
    mlfq_dequeue(task);

    /* Enqueue at next lower level. */
    int new_level = current_level + 1;
    mlfq_enqueue(task, new_level);

    mlfq_log("[MLFQ] Decay: PID=");
    mlfq_log_u64(task->pid);
    mlfq_log(" L");
    mlfq_log_u64(current_level);
    mlfq_log(" -> L");
    mlfq_log_u64(new_level);
    mlfq_log("\r\n");
}

void mlfq_boost_all(void) {
    /* Collect all tasks from levels 1-3. */
    task_t *collect_head = NULL;
    task_t *collect_tail = NULL;
    int collected = 0;

    for (int level = 1; level < MLFQ_LEVELS; level++) {
        mlfd_queue_t *q = &mlfq_queues[level];
        if (q->head == NULL) continue;

        /* Append entire list. */
        if (collect_tail) {
            collect_tail->next = q->head;
        } else {
            collect_head = q->head;
        }
        collect_tail = q->tail;
        collected += q->count;

        /* Clear queue. */
        q->head = NULL;
        q->tail = NULL;
        q->count = 0;
    }

    /* Clear bitmap levels 1-3. */
    mlfq_bitmap &= 0x1;  /* Keep only level 0 bit. */

    /* Re-enqueue collected tasks at level 0. */
    if (collect_head) {
        mlfd_queue_t *q0 = &mlfq_queues[0];

        task_t *t = collect_head;
        while (t) {
            task_t *next = t->next;
            t->mlfq_level = 0;
            t->remaining_ticks = mlfq_quanta[0];
            t->time_slice = mlfq_quanta[0];
            t->next = NULL;

            if (q0->tail) {
                q0->tail->next = t;
            } else {
                q0->head = t;
            }
            q0->tail = t;
            q0->count++;
            t = next;
        }

        mlfq_bitmap |= 1;  /* Set level 0 bit. */
    }

    if (collected > 0) {
        mlfq_log("[MLFQ] Boost: promoted ");
        mlfq_log_u64(collected);
        mlfq_log(" tasks to L0\r\n");
    }

    g_ticks_since_boost = 0;
}

uint32_t mlfq_total_count(void) {
    uint32_t total = 0;
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        total += mlfq_queues[i].count;
    }
    return total;
}

uint64_t mlfq_get_ticks_since_boost(void) {
    return g_ticks_since_boost;
}

void mlfq_tick(void) {
    g_ticks_since_boost++;

    /* Periodic priority boost. */
    if (g_ticks_since_boost >= MLFQ_BOOST_INTERVAL) {
        mlfq_boost_all();
    }
}

void mlfq_dump(void) {
    if (!g_mlfq_serial) return;

    mlfq_log("[MLFQ] bitmap=");
    mlfq_log_u64(mlfq_bitmap);
    mlfq_log(" total=");
    mlfq_log_u64(mlfq_total_count());
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        mlfq_log(" L");
        mlfq_log_u64(i);
        mlfq_log("=");
        mlfq_log_u64(mlfq_queues[i].count);
    }
    mlfq_log("\r\n");
}
