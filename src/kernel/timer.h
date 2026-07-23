/**
 * @file timer.h
 * @brief System timer — tick counter and IRQ 0 handler interface.
 *
 * The timer module provides the OS heartbeat:
 *   - A monotonically increasing tick counter (incremented on every IRQ 0).
 *   - A busy-wait delay function for early-boot use.
 *   - Integration point with the scheduler: each tick calls
 *     scheduler_tick() to drive preemption.
 *
 * IRQ 0 is delivered as INT 32 after PIC remap. The PIT Channel 0
 * is programmed at initialization to fire at the desired frequency.
 *
 * Init order:
 *   pit_init(freq) → timer_init() → [scheduler_init()] → sti
 *
 * After sti, IRQ 0 fires at the programmed rate, incrementing the
 * global tick counter on each interrupt.
 */

#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H

#include <stdint.h>
#include <stdbool.h>
#include "isr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the timer subsystem.
 *
 * Programs the PIT at the default frequency (100 Hz), registers the
 * IRQ 0 handler on IDT vector 32, and unmasks IRQ 0 on the PIC.
 *
 * Must be called after pic_init() and idt_init().
 * After timer_init(), interrupts must be enabled (sti) for ticks
 * to actually occur.
 */
void timer_init(void *serial_dev);

/**
 * @brief Get the current tick count.
 *
 * @return Number of timer interrupts that have fired since timer_init().
 */
uint64_t timer_get_ticks(void);

/**
 * @brief Busy-wait for approximately the given number of ticks.
 *
 * Spins in a loop reading timer_get_ticks(). This is a blocking
 * wait and should only be used before the scheduler is active.
 * After the scheduler starts, use task_sleep() instead.
 *
 * @param ticks  Number of ticks to wait.
 */
void timer_wait_ticks(uint64_t ticks);

/**
 * @brief Timer IRQ 0 interrupt handler (vector 32).
 *
 * Called from isr_common_handler on every IRQ 0. Increments the
 * global tick counter and (once available) calls scheduler_tick().
 *
 * @param frame  Interrupt frame from the CPU.
 */
void timer_irq_handler(interrupt_frame_t *frame);

/**
 * @brief Check whether the timer subsystem has been initialized.
 *
 * @return true if timer_init() has been called.
 */
bool timer_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_TIMER_H */
