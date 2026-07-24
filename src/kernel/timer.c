/**
 * @file timer.c
 * @brief System timer implementation.
 *
 * Provides the OS heartbeat via PIT Channel 0 (IRQ 0, vector 32).
 * Each tick increments a global counter and optionally drives the
 * scheduler for preemptive multitasking.
 */

#include "timer.h"
#include "pic.h"
#include "isr.h"
#include "../drivers/pit.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Static state
 * ========================================================================= */

/** Monotonic tick counter — incremented on every IRQ 0. */
static volatile uint64_t g_ticks = 0;

/** Serial device for logging. */
static serial_dev_t *g_timer_serial = NULL;

/** True after timer_init() completes. */
static bool g_timer_initialized = false;

/* =========================================================================
 * Forward declaration — scheduler_tick() is defined in scheduler.c.
 * We declare it here as weak to avoid a hard link dependency during
 * early compilation phases. The scheduler will override this. ========================================================================= */

/**
 * @brief Called from the timer handler on every tick to drive preemption.
 *
 * This weak symbol is overridden by scheduler_tick() in scheduler.c
 * when the scheduler is linked. Until then, it is a no-op.
 */
__attribute__((weak))
void scheduler_tick(interrupt_frame_t *frame) {
    (void)frame;
}

/* =========================================================================
 * Helper — serial output
 * ========================================================================= */

static void timer_log(const char *msg) {
    if (g_timer_serial) {
        serial_write_string(g_timer_serial, msg);
    }
}

static void timer_log_hex(uint64_t val) {
    if (!g_timer_serial) return;
    static const char hex[] = "0123456789ABCDEF";
    serial_write_string(g_timer_serial, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_write_char(g_timer_serial, hex[(val >> i) & 0xF]);
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void timer_init(void *serial_dev) {
    g_timer_serial = (serial_dev_t *)serial_dev;

    timer_log("[INFO] Timer initializing...\r\n");

    /* Program PIT Channel 0 at the default frequency. */
    pit_init(PIT_DEFAULT_FREQ);

    uint32_t freq = pit_get_frequency();
    timer_log("[INFO] PIT programmed at ");
    {
        char buf[12];
        int i = 0;
        uint32_t f = freq;
        if (f == 0) { buf[i++] = '0'; }
        else {
            char tmp[12];
            int j = 0;
            while (f > 0) { tmp[j++] = '0' + (f % 10); f /= 10; }
            while (j > 0) { buf[i++] = tmp[--j]; }
        }
        buf[i] = '\0';
        timer_log(buf);
    }
    timer_log(" Hz\r\n");

    /* Register our handler for IRQ 0 → INT 32 (after PIC remap). */
    isr_register_handler(32, timer_irq_handler);

    /* Unmask IRQ 0 on the master PIC to allow timer interrupts. */
    pic_unmask_irq(0);

    g_timer_initialized = true;
    timer_log("[INFO] Timer initialized (IRQ 0 unmasked, vector 32).\r\n");
}

uint64_t timer_get_ticks(void) {
    return g_ticks;
}

void timer_wait_ticks(uint64_t ticks) {
    /* Verify interrupts are enabled — hlt will deadlock if IF=0. */
    uint64_t rflags;
    asm volatile ("pushfq; pop %0" : "=r"(rflags));
    if (!(rflags & 0x200)) {
        timer_log("[TIMER] WARN: timer_wait_ticks called with interrupts disabled!\r\n");
        /* Fall back to busy-wait instead of deadlocking. */
        uint64_t target = g_ticks + ticks;
        while (g_ticks < target) {
            asm volatile ("pause");
        }
        return;
    }

    uint64_t target = g_ticks + ticks;
    while (g_ticks < target) {
        asm volatile ("hlt");  /* Save power until next interrupt. */
    }
}

void timer_irq_handler(interrupt_frame_t *frame) {
    g_ticks++;

    /* Drive the scheduler — this weak symbol becomes scheduler_tick()
     * when the scheduler module is linked. */
    scheduler_tick(frame);
}

bool timer_is_initialized(void) {
    return g_timer_initialized;
}
