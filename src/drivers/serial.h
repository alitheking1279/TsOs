/**
 * @file serial.h
 * @brief Minimal, defensive driver for 16550-compatible UARTs on x86 PC/AT
 *        style I/O ports (COM1-COM4).
 *
 * Register map (offset from each port's base address), per the standard
 * 16550 UART programming model:
 *
 *   Offset  DLAB=0                  DLAB=1
 *   ------  ----------------------  ----------------------
 *   +0      RBR (read) / THR (write) DLL (divisor latch low)
 *   +1      IER (interrupt enable)   DLM (divisor latch high)
 *   +2      IIR (read) / FCR (write) --
 *   +3      LCR (line control)       --
 *   +4      MCR (modem control)      --
 *   +5      LSR (line status)        --
 *   +6      MSR (modem status)       --
 *   +7      Scratch register        --
 *
 * DLAB = Divisor Latch Access Bit, LCR bit 7.
 */

#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Standard PC/AT COM port base I/O addresses. */
typedef enum {
    SERIAL_COM1 = 0x3F8,
    SERIAL_COM2 = 0x2F8,
    SERIAL_COM3 = 0x3E8,
    SERIAL_COM4 = 0x2E8
} serial_port_t;

/** Return/status codes. Every public function reports failure explicitly;
 *  nothing is allowed to silently hang or silently drop data. */
typedef enum {
    SERIAL_OK               =  0,
    SERIAL_ERR_LOOPBACK      = -1, /**< Loopback self-test failed: no UART or dead hardware at this port. */
    SERIAL_ERR_TIMEOUT       = -2, /**< Hardware did not become ready within the bounded retry budget. */
    SERIAL_ERR_INVALID_ARG   = -3, /**< NULL pointer or otherwise invalid argument. */
    SERIAL_ERR_NOT_INITIALIZED = -4 /**< serial_init() was never called or failed for this port. */
} serial_status_t;

/** Opaque-ish handle: one per physical port. Callers should treat the
 *  internals as private; the struct is exposed here only so it can live
 *  on the stack/BSS without dynamic allocation (no heap in early boot). */
typedef struct {
    uint16_t base;      /**< I/O port base address, e.g. SERIAL_COM1. */
    int      ready;      /**< Nonzero once serial_init() has succeeded. */
} serial_dev_t;

/**
 * @brief Initialize a UART port: program baud rate, framing, FIFOs, and
 *        verify the hardware is actually present via internal loopback.
 *
 * @param dev  Caller-owned handle to populate. Must not be NULL.
 * @param port Which COM port to bring up.
 * @return SERIAL_OK on success; SERIAL_ERR_LOOPBACK if no UART responds
 *         correctly at that address; SERIAL_ERR_INVALID_ARG if dev is NULL.
 *
 * @note On failure, dev->ready is left at 0 and all other calls on this
 *       handle will return SERIAL_ERR_NOT_INITIALIZED rather than touching
 *       hardware.
 */
serial_status_t serial_init(serial_dev_t *dev, serial_port_t port);

/**
 * @brief Write a single byte, blocking until the transmitter is ready.
 * @return SERIAL_OK, SERIAL_ERR_NOT_INITIALIZED, or SERIAL_ERR_TIMEOUT if
 *         the hardware never reports "transmit empty" within the retry budget.
 */
serial_status_t serial_write_char(serial_dev_t *dev, char c);

/**
 * @brief Write a NUL-terminated string. Stops at the first byte that fails.
 * @return SERIAL_OK, SERIAL_ERR_INVALID_ARG (dev or str NULL),
 *         SERIAL_ERR_NOT_INITIALIZED, or SERIAL_ERR_TIMEOUT.
 */
serial_status_t serial_write_string(serial_dev_t *dev, const char *str);

/**
 * @brief Non-blocking check for whether a received byte is waiting.
 * @return 1 if data is available, 0 if not, negative serial_status_t on error.
 */
int serial_data_available(serial_dev_t *dev);

/**
 * @brief Read one byte. Blocks (with bounded timeout) until data arrives.
 * @param out Destination for the received byte. Must not be NULL.
 */
serial_status_t serial_read_char(serial_dev_t *dev, char *out);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_SERIAL_H */