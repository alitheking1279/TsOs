/**
 * @file pit.h
 * @brief Intel 8253/8254 Programmable Interval Timer (PIT) driver interface.
 *
 * The PIT is the simplest x86 timer source. It has three independent
 * 16-bit countdown channels connected to I/O ports 0x40-0x42:
 *
 *   Channel 0 — Connected to PIC IRQ 0 (INT 32). Used for the system tick.
 *   Channel 1 — Historically for DRAM refresh. Not used on modern PCs.
 *   Channel 2 — Connected to the PC speaker.
 *
 * Channel 0 is the only one we use. It counts down from a programmed
 * reload value at the PIT's fixed input frequency (1,193,182 Hz).
 * When it reaches zero, it asserts IRQ 0 and reloads automatically.
 *
 * Programming model (Intel 8253 datasheet, Mode 2 — Rate Generator):
 *   - Send a command byte to port 0x43 to select channel and mode.
 *   - Send the 16-bit divisor in two bytes (low then high) to port 0x40.
 *   - Channel 0 counts down at 1,193,182 Hz / divisor.
 *   - On reaching 0, it asserts IRQ 0 and reloads from the divisor.
 *
 * Command byte layout:
 *   bit 7-6: Channel select (00 = channel 0)
 *   bit 5-4: Access mode (11 = lobyte/hibyte)
 *   bit 3-1: Operating mode (010 = Mode 2, rate generator)
 *   bit 0:   BCD/Binary (0 = binary)
 *   → Command byte for channel 0, Mode 2, lobyte/hibyte = 0x34
 *
 * References:
 *   Intel 8253 Programmable Interval Timer datasheet
 *   OSDev wiki — PIT (Programmable Interval Timer)
 */

#ifndef DRIVERS_PIT_H
#define DRIVERS_PIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** PIT input oscillator frequency in Hz (Intel spec: 1,193,182 Hz). */
#define PIT_BASE_FREQ       1193182UL

/** I/O port for Channel 0 data (read/write divisor). */
#define PIT_CHANNEL0_PORT   0x40

/** I/O port for Channel 1 data. */
#define PIT_CHANNEL1_PORT   0x41

/** I/O port for Channel 2 data. */
#define PIT_CHANNEL2_PORT   0x42

/** I/O port for PIT command register. */
#define PIT_COMMAND_PORT    0x43

/** Default tick frequency in Hz (100 Hz = 10 ms per tick). */
#define PIT_DEFAULT_FREQ    100

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the PIT and program Channel 0.
 *
 * Programs Channel 0 in Mode 2 (Rate Generator) with the given tick
 * frequency. The resulting divisor is PIT_BASE_FREQ / freq.
 *
 * @param freq  Desired tick frequency in Hz (e.g. 100 for 10 ms ticks).
 *              Must be > 0 and <= PIT_BASE_FREQ.
 */
void pit_init(uint32_t freq);

/**
 * @brief Get the currently programmed tick frequency.
 *
 * @return Frequency in Hz that Channel 0 was programmed to.
 */
uint32_t pit_get_frequency(void);

/**
 * @brief Get the current PIT channel 0 counter value.
 *
 * Uses the latch command (0x00) to read the current countdown value
 * without side effects.
 *
 * @return Current 16-bit counter value.
 */
uint16_t pit_get_counter(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_PIT_H */
