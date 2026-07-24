/**
 * @file portio.h
 * @brief Shared x86 port I/O primitives (inb/outb/io_wait).
 *
 * Consolidates the identical static-inline definitions previously
 * duplicated in serial.c and pic.c into a single authoritative header.
 *
 * Intel SDM Vol.2A §IN/OUT: All port I/O uses the IN and OUT
 * instructions with 8-bit data (AL) and 16-bit port addresses.
 */

#ifndef DRIVERS_PORTIO_H
#define DRIVERS_PORTIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write a byte to an I/O port.
 *
 * @param port  16-bit I/O port address.
 * @param val   8-bit value to write.
 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief Read a byte from an I/O port.
 *
 * @param port  16-bit I/O port address.
 * @return      8-bit value read.
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * @brief Write a 16-bit word to an I/O port.
 *
 * Intel SDM Vol.2A §OUT: Out a word from AX to a port addressed by DX.
 * Used by ATA PIO for sector data transfers (256 words per sector).
 *
 * @param port  16-bit I/O port address.
 * @param val   16-bit value to write.
 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief Read a 16-bit word from an I/O port.
 *
 * Intel SDM Vol.2A §IN: In a word from a port addressed by DX into AX.
 * Used by ATA PIO for sector data transfers (256 words per sector).
 *
 * @param port  16-bit I/O port address.
 * @return      16-bit value read.
 */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * @brief Burn ~1 microsecond by writing to port 0x80 (BIOS POST diagnostic).
 *
 * Required by the 8259A PIC datasheet between consecutive ICW writes.
 * Harmless on QEMU but critical for bare-metal hardware.
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_PORTIO_H */
