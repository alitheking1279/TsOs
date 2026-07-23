/**
 * @file pit.c
 * @brief Intel 8253/8254 PIT Channel 0 driver implementation.
 *
 * Programs Channel 0 as a rate generator (Mode 2) to produce a
 * periodic IRQ 0 at the desired tick frequency.
 *
 * Mode 2 behavior:
 *   - The counter reloads from the programmed divisor.
 *   - Counts down at PIT_BASE_FREQ / divisor Hz.
 *   - When it reaches 1, it asserts IRQ 0 for one clock cycle.
 *   - At 0, it reloads and starts again.
 *
 * The divisor must fit in 16 bits (0-65535). A divisor of 0 is
 * treated as 65536 (maximum period). The minimum useful frequency
 * is PIT_BASE_FREQ / 65535 ≈ 18.2 Hz.
 */

#include "pit.h"
#include "../drivers/portio.h"

/** Command byte: channel 0, lobyte/hibyte access, mode 2, binary. */
#define PIT_CMD_CHANNEL0_MODE2  0x34

/** Latch command for channel 0: freeze counter for reading.
 *  Intel 8253 datasheet: channel=00, access=00 (latch), mode=000, BCD=0 → 0x00. */
#define PIT_CMD_LATCH_CH0       0x00

/** Current programmed frequency (set during pit_init). */
static uint32_t g_pit_frequency = 0;

void pit_init(uint32_t freq) {
    if (freq == 0) freq = PIT_DEFAULT_FREQ;
    if (freq > PIT_BASE_FREQ) freq = PIT_BASE_FREQ;

    /* Calculate the reload divisor.
     * Intel SDM: divisor = input_freq / desired_freq.
     * A divisor of 0 means 65536 (hardware wraps). */
    uint32_t divisor = PIT_BASE_FREQ / freq;
    if (divisor == 0) divisor = 1;
    if (divisor > 65536) divisor = 65536;

    uint8_t divisor_lo = (uint8_t)(divisor & 0xFF);
    uint8_t divisor_hi = (uint8_t)((divisor >> 8) & 0xFF);

    /* Send command byte: select channel 0, lobyte/hibyte, mode 2, binary. */
    outb(PIT_COMMAND_PORT, PIT_CMD_CHANNEL0_MODE2);

    /* Send divisor (low byte first, then high byte). */
    outb(PIT_CHANNEL0_PORT, divisor_lo);
    outb(PIT_CHANNEL0_PORT, divisor_hi);

    g_pit_frequency = freq;
}

uint32_t pit_get_frequency(void) {
    return g_pit_frequency;
}

uint16_t pit_get_counter(void) {
    /* Send latch command to freeze channel 0 counter without side effects.
     * The next two reads from channel 0 port return low then high bytes. */
    outb(PIT_COMMAND_PORT, PIT_CMD_LATCH_CH0);

    uint16_t counter = (uint16_t)inb(PIT_CHANNEL0_PORT);
    counter |= (uint16_t)(inb(PIT_CHANNEL0_PORT) << 8);

    return counter;
}
