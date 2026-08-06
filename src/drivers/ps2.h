/**
 * @file ps2.h
 * @brief IBM PS/2 Keyboard driver interface (Scancode Set 1).
 *
 * The PS/2 keyboard controller uses two I/O ports:
 *   Port 0x60 — Data port (read scancodes, write controller data)
 *   Port 0x64 — Status/command port (read status, write commands)
 *
 * IRQ 1 (INT 33 after PIC remap) fires on every key press/release.
 * The driver translates Scancode Set 1 make/break codes to ASCII,
 * tracks modifier state (Shift, Ctrl, Alt, CapsLock), and provides
 * a 256-byte ring buffer for decoded characters.
 *
 * Keyboard controller commands:
 *   0xAE — Enable keyboard
 *   0xAD — Disable keyboard
 *   0xF5 — Disable scanning
 *   0xF4 — Enable scanning
 *   0xF2 — Identify keyboard
 *
 * Scancode Set 1 make codes: 0x01-0x58 (key pressed)
 * Scancode Set 1 break codes: make | 0x80 (key released)
 * Extended prefix: 0xE0 (e.g. arrow keys, right Ctrl/Alt)
 *
 * References:
 *   Intel 8259A PIC datasheet
 *   OSDev wiki — PS/2 Keyboard
 *   OSDev wiki — Scancode Set 1
 */

#ifndef DRIVERS_PS2_H
#define DRIVERS_PS2_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * I/O Port Addresses
 * ========================================================================= */

/** PS/2 data port — read scancodes, write controller data. */
#define PS2_DATA_PORT       0x60

/** PS/2 status/command port — read controller status, send commands. */
#define PS2_STATUS_PORT     0x64

/* =========================================================================
 * Controller Commands
 * ========================================================================= */

/** Enable the keyboard device. */
#define PS2_CMD_ENABLE_KB       0xAE

/** Disable the keyboard device. */
#define PS2_CMD_DISABLE_KB      0xAD

/** Disable keyboard scanning (stop reporting key events). */
#define PS2_CMD_DISABLE_SCAN    0xF5

/** Enable keyboard scanning (resume reporting key events). */
#define PS2_CMD_ENABLE_SCAN     0xF4

/** Request keyboard identification. */
#define PS2_CMD_IDENTIFY        0xF2

/** Reset the keyboard. */
#define PS2_CMD_RESET           0xFF

/* =========================================================================
 * Status Register Bits
 * ========================================================================= */

/** Bit 0: Output buffer full (data available at port 0x60). */
#define PS2_STATUS_OBF          (1 << 0)

/** Bit 1: Input buffer full (controller busy, don't write). */
#define PS2_STATUS_IBF          (1 << 1)

/** Bit 4: Timeout error. */
#define PS2_STATUS_TIMEOUT      (1 << 6)

/** Bit 5: Parity error. */
#define PS2_STATUS_PARITY       (1 << 7)

/* =========================================================================
 * Modifier Key Bitmask
 * ========================================================================= */

/** Left Shift held. */
#define PS2_MOD_LSHIFT      (1 << 0)

/** Right Shift held. */
#define PS2_MOD_RSHIFT      (1 << 1)

/** Left Ctrl held. */
#define PS2_MOD_LCTRL       (1 << 2)

/** Right Ctrl held. */
#define PS2_MOD_RCTRL       (1 << 3)

/** Left Alt held. */
#define PS2_MOD_LALT        (1 << 4)

/** Right Alt held. */
#define PS2_MOD_RALT        (1 << 5)

/** CapsLock active (toggled). */
#define PS2_MOD_CAPSLOCK    (1 << 6)

/** Combined Shift active (LSHIFT | RSHIFT). */
#define PS2_MOD_SHIFT       (PS2_MOD_LSHIFT | PS2_MOD_RSHIFT)

/** Combined Ctrl active (LCTRL | RCTRL). */
#define PS2_MOD_CTRL        (PS2_MOD_LCTRL | PS2_MOD_RCTRL)

/** Combined Alt active (LALT | RALT). */
#define PS2_MOD_ALT         (PS2_MOD_LALT | PS2_MOD_RALT)

/* =========================================================================
 * Special Key Codes (returned by ps2_get_key, non-ASCII)
 * ========================================================================= */

#define PS2_KEY_UP          0x80
#define PS2_KEY_DOWN        0x81
#define PS2_KEY_LEFT        0x82
#define PS2_KEY_RIGHT       0x83
#define PS2_KEY_F1          0x84
#define PS2_KEY_F2          0x85
#define PS2_KEY_F3          0x86
#define PS2_KEY_F4          0x87
#define PS2_KEY_F5          0x88
#define PS2_KEY_F6          0x89
#define PS2_KEY_F7          0x8A
#define PS2_KEY_F8          0x8B
#define PS2_KEY_F9          0x8C
#define PS2_KEY_F10         0x8D
#define PS2_KEY_F11         0x8E
#define PS2_KEY_F12         0x8F
#define PS2_KEY_HOME        0x90
#define PS2_KEY_END         0x91
#define PS2_KEY_PAGE_UP     0x92
#define PS2_KEY_PAGE_DOWN   0x93
#define PS2_KEY_INSERT      0x94
#define PS2_KEY_DELETE      0x95
#define PS2_KEY_ENTER       '\r'
#define PS2_KEY_TAB         '\t'
#define PS2_KEY_BACKSPACE   0x08
#define PS2_KEY_ESC         0x1B

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the PS/2 keyboard controller and register IRQ 1 handler.
 *
 * Performs:
 *   1. Disable keyboard (command 0xAD)
 *   2. Flush output buffer (read port 0x60)
 *   3. Disable scanning (command 0xF5)
 *   4. Reset keyboard (command 0xFF) with response check
 *   5. Enable scanning (command 0xF4)
 *   6. Register IRQ 1 handler on vector 33
 *   7. Unmask IRQ 1 on PIC
 *
 * @param serial_dev  Serial device for debug logging. May be NULL.
 */
void ps2_init(void *serial_dev);

/**
 * @brief Read one decoded character from the keyboard ring buffer.
 *
 * Blocks if the buffer is empty (spins with HLT in a bounded loop).
 *
 * @return ASCII character (0x20-0x7E), special key code (PS2_KEY_*),
 *         or 0 on timeout/error.
 */
uint8_t ps2_get_key(void);

/**
 * @brief Non-blocking check for available characters.
 *
 * @return true if at least one character is available, false otherwise.
 */
bool ps2_key_available(void);

/**
 * @brief Get the current modifier key state bitmask.
 *
 * @return Bitmask of PS2_MOD_* flags.
 */
uint8_t ps2_get_modifiers(void);

/**
 * @brief Blocking read with configurable timeout (milliseconds).
 *
 * Polls the ring buffer using timer ticks for real-time timeout.
 * Falls back to polling the PS/2 status port if IRQ delivery fails.
 * Returns 0 if no key pressed within timeout_ms.
 *
 * @param timeout_ms  Timeout in milliseconds.
 * @return Decoded key or 0 on timeout.
 */
uint8_t ps2_get_key_timeout(uint32_t timeout_ms);

/**
 * @brief Get the number of keyboard IRQs that have fired since init.
 *
 * Useful for diagnostics — if this stays 0, IRQ 1 is not being delivered.
 *
 * @return IRQ invocation count.
 */
uint32_t ps2_get_irq_count(void);

/**
 * @brief Get the number of OBF (Output Buffer Full) polling hits.
 *
 * Useful for diagnostics — if this stays 0, the PS/2 controller never
 * produced output (keyboard not sending data or not connected).
 *
 * @return OBF hit count.
 */
uint32_t ps2_get_obf_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_PS2_H */
