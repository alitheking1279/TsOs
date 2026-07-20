/**
 * @file serial.c
 * @brief Implementation of the 16550 UART driver declared in serial.h.
 *
 * Design goals:
 *  - No unbounded busy-wait: every hardware poll loop has a retry cap and
 *    returns SERIAL_ERR_TIMEOUT instead of hanging the kernel forever.
 *  - No hidden global state: every function operates on a caller-owned
 *    serial_dev_t, so multiple ports can be driven independently.
 *  - Fail loudly: every function checks dev->ready before touching I/O
 *    ports, so a forgotten serial_init() call is a clean error, not
 *    undefined behavior on an unprogrammed port.
 */

#include "serial.h"

/* ---- Register offsets (relative to port base) ------------------------- */
#define REG_DATA        0 /**< DLAB=0: RBR (read) / THR (write) */
#define REG_IER         1 /**< DLAB=0: Interrupt Enable Register */
#define REG_DLL         0 /**< DLAB=1: Divisor Latch Low byte */
#define REG_DLM         1 /**< DLAB=1: Divisor Latch High byte */
#define REG_FCR         2 /**< FIFO Control Register (write) */
#define REG_LCR         3 /**< Line Control Register */
#define REG_MCR         4 /**< Modem Control Register */
#define REG_LSR         5 /**< Line Status Register */

/* ---- LCR bits ----------------------------------------------------------*/
#define LCR_WORD_LEN_8      0x03 /**< Bits 0-1: word length = 8 data bits */
#define LCR_STOP_BITS_1     0x00 /**< Bit 2: 0 = one stop bit */
#define LCR_PARITY_NONE     0x00 /**< Bits 3-5: no parity */
#define LCR_DLAB            0x80 /**< Bit 7: Divisor Latch Access Bit */

/* ---- FCR bits ------------------------------------------------------------
 * 0xC7 = FIFO_ENABLE | CLEAR_RX | CLEAR_TX | TRIGGER_14 */
#define FCR_ENABLE_FIFO     0x01
#define FCR_CLEAR_RX_FIFO   0x02
#define FCR_CLEAR_TX_FIFO   0x04
#define FCR_TRIGGER_14BYTE  0xC0
#define FCR_STANDARD_CONFIG (FCR_ENABLE_FIFO | FCR_CLEAR_RX_FIFO | \
                             FCR_CLEAR_TX_FIFO | FCR_TRIGGER_14BYTE)

/* ---- MCR bits ----------------------------------------------------------*/
#define MCR_DTR             0x01
#define MCR_RTS             0x02
#define MCR_OUT1            0x04
#define MCR_OUT2            0x08 /**< Must be set to enable IRQs on real HW */
#define MCR_LOOPBACK        0x10 /**< Internal loopback, for self-test only */
#define MCR_NORMAL_OPERATION (MCR_DTR | MCR_RTS | MCR_OUT1 | MCR_OUT2)
#define MCR_LOOPBACK_TEST     (MCR_DTR | MCR_RTS | MCR_OUT1 | MCR_OUT2 | MCR_LOOPBACK)

/* ---- LSR bits ------------------------------------------------------------
 * Only the bits this driver uses are named; the rest are reserved for
 * future error-reporting work (see serial_get_line_status(), not yet
 * implemented). */
#define LSR_DATA_READY      0x01 /**< Bit 0: a byte is waiting in RBR */
#define LSR_TRANSMIT_EMPTY  0x20 /**< Bit 5: THR is empty, safe to write */

/** Baud rate divisor for 38400 baud, given the UART's fixed 115200 Hz
 *  input clock (divisor = 115200 / desired_baud). This value, its high
 *  byte, and the resulting baud rate are fixed by the hardware, not
 *  chosen arbitrarily. */
#define BAUD_DIVISOR_38400_LOW   0x03
#define BAUD_DIVISOR_38400_HIGH  0x00

/** A test byte with no special meaning, used only to verify that a byte
 *  written into internal loopback comes back unchanged. */
#define LOOPBACK_TEST_BYTE       0xAE

/** Bound on hardware-status polling so a dead/missing UART produces a
 *  timeout error instead of an infinite loop. This is a spin count, not
 *  a calibrated time value -- there is no timer dependency here, which
 *  matters because this driver may run before any timer is set up. */
#define POLL_RETRY_LIMIT         100000

/* ---- Low-level port I/O ------------------------------------------------*/

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

/* ---- Internal helpers ----------------------------------------------------
 * These take the raw base address rather than serial_dev_t so serial_init()
 * can use them before dev->ready is set. */

static uint8_t read_lsr(uint16_t base) {
    return inb(base + REG_LSR);
}

/** Poll until (lsr & mask) == mask, or give up after POLL_RETRY_LIMIT
 *  iterations. Returns SERIAL_OK or SERIAL_ERR_TIMEOUT. */
static serial_status_t wait_for_lsr_bit(uint16_t base, uint8_t mask) {
    for (uint32_t i = 0; i < POLL_RETRY_LIMIT; i++) {
        if ((read_lsr(base) & mask) == mask) {
            return SERIAL_OK;
        }
    }
    return SERIAL_ERR_TIMEOUT;
}

/* ---- Public API ----------------------------------------------------------*/

serial_status_t serial_init(serial_dev_t *dev, serial_port_t port) {
    if (dev == NULL) {
        return SERIAL_ERR_INVALID_ARG;
    }

    dev->base  = (uint16_t)port;
    dev->ready = 0;

    const uint16_t base = dev->base;

    outb(base + REG_IER, 0x00); /* Disable all UART interrupts during setup. */

    /* Program the baud rate divisor. This requires DLAB=1, which
     * temporarily changes what offsets 0 and 1 mean. */
    outb(base + REG_LCR, LCR_DLAB);
    outb(base + REG_DLL, BAUD_DIVISOR_38400_LOW);
    outb(base + REG_DLM, BAUD_DIVISOR_38400_HIGH);

    /* Set frame format (8N1) and clear DLAB. */
    outb(base + REG_LCR, LCR_WORD_LEN_8 | LCR_STOP_BITS_1 | LCR_PARITY_NONE);

    outb(base + REG_FCR, FCR_STANDARD_CONFIG);

    /* Put the UART into internal loopback so we can verify a real 16550
     * (or compatible) is actually present at this address before trusting
     * it for real I/O. */
    outb(base + REG_MCR, MCR_LOOPBACK_TEST);
    outb(base + REG_DATA, LOOPBACK_TEST_BYTE);

    /* In loopback mode transmitted bytes are looped straight back into
     * the receiver, so this read reflects what we just wrote, not
     * external wiring. */
    if (inb(base + REG_DATA) != LOOPBACK_TEST_BYTE) {
        return SERIAL_ERR_LOOPBACK;
    }

    /* Loopback test passed; switch to normal operation. */
    outb(base + REG_MCR, MCR_NORMAL_OPERATION);

    dev->ready = 1;
    return SERIAL_OK;
}

serial_status_t serial_write_char(serial_dev_t *dev, char c) {
    if (dev == NULL) {
        return SERIAL_ERR_INVALID_ARG;
    }
    if (!dev->ready) {
        return SERIAL_ERR_NOT_INITIALIZED;
    }

    serial_status_t st = wait_for_lsr_bit(dev->base, LSR_TRANSMIT_EMPTY);
    if (st != SERIAL_OK) {
        return st;
    }

    outb(dev->base + REG_DATA, (uint8_t)c);
    return SERIAL_OK;
}

serial_status_t serial_write_string(serial_dev_t *dev, const char *str) {
    if (dev == NULL || str == NULL) {
        return SERIAL_ERR_INVALID_ARG;
    }

    for (size_t i = 0; str[i] != '\0'; i++) {
        serial_status_t st = serial_write_char(dev, str[i]);
        if (st != SERIAL_OK) {
            return st; /* Stop at first failure rather than losing data silently. */
        }
    }
    return SERIAL_OK;
}

int serial_data_available(serial_dev_t *dev) {
    if (dev == NULL) {
        return SERIAL_ERR_INVALID_ARG;
    }
    if (!dev->ready) {
        return SERIAL_ERR_NOT_INITIALIZED;
    }
    return (read_lsr(dev->base) & LSR_DATA_READY) ? 1 : 0;
}

serial_status_t serial_read_char(serial_dev_t *dev, char *out) {
    if (dev == NULL || out == NULL) {
        return SERIAL_ERR_INVALID_ARG;
    }
    if (!dev->ready) {
        return SERIAL_ERR_NOT_INITIALIZED;
    }

    serial_status_t st = wait_for_lsr_bit(dev->base, LSR_DATA_READY);
    if (st != SERIAL_OK) {
        return st;
    }

    *out = (char)inb(dev->base + REG_DATA);
    return SERIAL_OK;
}