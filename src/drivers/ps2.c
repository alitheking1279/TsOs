/**
 * @file ps2.c
 * @brief IBM PS/2 Keyboard driver implementation (Scancode Set 1).
 *
 * Handles IRQ 1 (INT 33) for keyboard input. Translates Scancode Set 1
 * make/break codes to ASCII characters and special key codes. Tracks
 * modifier key state and provides a ring buffer for decoded input.
 *
 * Keyboard controller initialization sequence:
 *   1. Disable keyboard (0xAD)
 *   2. Flush output buffer
 *   3. Disable scanning (0xF5)
 *   4. Reset keyboard (0xFF) — expect 0xAA response
 *   5. Enable scanning (0xF4)
 *
 * Scancode Set 1 format:
 *   Make code:  0x01-0x58 (key pressed)
 *   Break code: make | 0x80 (key released)
 *   Extended:   0xE0 prefix (arrow keys, right modifiers, etc.)
 *
 * References:
 *   OSDev wiki — PS/2 Keyboard
 *   OSDev wiki — Scancode Set 1
 */

#include "ps2.h"
#include "pit.h"
#include "portio.h"
#include "../kernel/isr.h"
#include "../kernel/pic.h"
#include "../kernel/timer.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Ring buffer capacity (must be power of 2 for fast masking). */
#define PS2_RING_SIZE       256

/** Ring buffer mask (PS2_RING_SIZE - 1). */
#define PS2_RING_MASK       (PS2_RING_SIZE - 1)

/** Polling timeout for keyboard controller responses. */
#define PS2_TIMEOUT         100000

/** IRQ 1 vector number (IRQ 1 + PIC1_VECTOR_OFFSET 0x20 = 0x21 = 33). */
#define PS2_IRQ_VECTOR      33

/** Keyboard IRQ line number. */
#define PS2_IRQ_LINE        1

/* =========================================================================
 * Scancode Set 1 — ASCII translation tables
 *
 * Index = scancode (0x00-0x57). Entry = ASCII character (0 = no mapping).
 * These are the MAKE (press) codes only. Break codes are handled by
 * inverting bit 7 and clearing modifier state.
 * ========================================================================= */

/* Scancode Set 1 unshifted: exactly 128 entries (0x00-0x7F). */
static const uint8_t sc1_unshifted[128] = {
    0,     PS2_KEY_ESC, '1',   '2',   '3',   '4',   '5',   '6', /* 0x00-0x07 */
    '7',   '8',   '9',   '0',   '-',   '=',   PS2_KEY_BACKSPACE,
                                       PS2_KEY_TAB,               /* 0x08-0x0F */
    'q',   'w',   'e',   'r',   't',   'y',   'u',   'i',     /* 0x10-0x17 */
    'o',   'p',   '[',   ']',   PS2_KEY_ENTER, 0,     'a',   's', /* 0x18-0x1F */
    'd',   'f',   'g',   'h',   'j',   'k',   'l',   ';',     /* 0x20-0x27 */
    '\'',  '`',   0,     '\\',  'z',   'x',   'c',   'v',     /* 0x28-0x2F */
    'b',   'n',   'm',   ',',   '.',   '/',   0,     '*',     /* 0x30-0x37 */
    0,     ' ',   0,     0,     0,     0,     0,     0,       /* 0x38-0x3F */
    PS2_KEY_F1,  PS2_KEY_F2,  PS2_KEY_F3,  PS2_KEY_F4,
    PS2_KEY_F5,  PS2_KEY_F6,  PS2_KEY_F7,  PS2_KEY_F8,        /* 0x40-0x47 */
    PS2_KEY_F9,  PS2_KEY_F10, 0,     0,     0,     0,     0,     0, /* 0x48-0x4F */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x50-0x57 */
    PS2_KEY_F11, PS2_KEY_F12, 0,     0,     0,     0,     0,     0, /* 0x58-0x5F */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x60-0x67 */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x68-0x6F */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x70-0x77 */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x78-0x7F */
};

/* Scancode Set 1 shifted: exactly 128 entries (0x00-0x7F). */
static const uint8_t sc1_shifted[128] = {
    0,     PS2_KEY_ESC, '!',   '@',   '#',   '$',   '%',   '^', /* 0x00-0x07 */
    '&',   '*',   '(',   ')',   '_',   '+',   PS2_KEY_BACKSPACE,
                                       PS2_KEY_TAB,               /* 0x08-0x0F */
    'Q',   'W',   'E',   'R',   'T',   'Y',   'U',   'I',     /* 0x10-0x17 */
    'O',   'P',   '{',   '}',   PS2_KEY_ENTER, 0,     'A',   'S', /* 0x18-0x1F */
    'D',   'F',   'G',   'H',   'J',   'K',   'L',   ':',     /* 0x20-0x27 */
    '"',   '~',   0,     '|',   'Z',   'X',   'C',   'V',     /* 0x28-0x2F */
    'B',   'N',   'M',   '<',   '>',   '?',   0,     '*',     /* 0x30-0x37 */
    0,     ' ',   0,     0,     0,     0,     0,     0,       /* 0x38-0x3F */
    PS2_KEY_F1,  PS2_KEY_F2,  PS2_KEY_F3,  PS2_KEY_F4,
    PS2_KEY_F5,  PS2_KEY_F6,  PS2_KEY_F7,  PS2_KEY_F8,        /* 0x40-0x47 */
    PS2_KEY_F9,  PS2_KEY_F10, 0,     0,     0,     0,     0,     0, /* 0x48-0x4F */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x50-0x57 */
    PS2_KEY_F11, PS2_KEY_F12, 0,     0,     0,     0,     0,     0, /* 0x58-0x5F */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x60-0x67 */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x68-0x6F */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x70-0x77 */
    0,     0,     0,     0,     0,     0,     0,     0,       /* 0x78-0x7F */
};

/* =========================================================================
 * Extended scancode table (0xE0 prefix)
 * ========================================================================= */

/** Extended make codes → special key codes. */
static const uint8_t ext_scancodes[128] = {
    [0x48] = PS2_KEY_UP,
    [0x50] = PS2_KEY_DOWN,
    [0x4B] = PS2_KEY_LEFT,
    [0x4D] = PS2_KEY_RIGHT,
    [0x47] = PS2_KEY_HOME,
    [0x4F] = PS2_KEY_END,
    [0x49] = PS2_KEY_PAGE_UP,
    [0x51] = PS2_KEY_PAGE_DOWN,
    [0x52] = PS2_KEY_INSERT,
    [0x53] = PS2_KEY_DELETE,
    [0x3B] = PS2_KEY_F1,
    [0x3C] = PS2_KEY_F2,
    [0x3D] = PS2_KEY_F3,
    [0x3E] = PS2_KEY_F4,
    [0x3F] = PS2_KEY_F5,
    [0x40] = PS2_KEY_F6,
    [0x41] = PS2_KEY_F7,
    [0x42] = PS2_KEY_F8,
    [0x43] = PS2_KEY_F9,
    [0x44] = PS2_KEY_F10,
    [0x57] = PS2_KEY_F11,
    [0x58] = PS2_KEY_F12,
    [0x1D] = PS2_MOD_RCTRL,
    [0x38] = PS2_MOD_RALT,
};

/* =========================================================================
 * State
 * ========================================================================= */

/** Ring buffer for decoded characters. */
static uint8_t  g_ring_buf[PS2_RING_SIZE];

/** Write index (ISR increments). */
static volatile uint32_t g_ring_head = 0;

/** Read index (consumer increments). */
static volatile uint32_t g_ring_tail = 0;

/** Current modifier key bitmask. */
static volatile uint8_t g_modifiers = 0;

/** Extended scancode prefix received (0xE0). */
static volatile bool g_extended = 0;

/** Serial device for debug logging. */
static serial_dev_t *g_ps2_serial = NULL;

/** Driver initialized flag. */
static bool g_initialized = false;

/** IRQ invocation counter (for diagnostics). */
static volatile uint32_t g_irq_count = 0;

/** Polling OBF hit counter (for diagnostics). */
static volatile uint32_t g_obf_count = 0;

/* =========================================================================
 * Ring Buffer Helpers
 * ========================================================================= */

/** Number of bytes in ring buffer. */
static inline uint32_t ring_count(void) {
    return (g_ring_head - g_ring_tail) & PS2_RING_MASK;
}

/** Push one byte into the ring buffer (called from ISR). */
static inline void ring_push(uint8_t val) {
    uint32_t next = (g_ring_head + 1) & PS2_RING_MASK;
    if (next != g_ring_tail) {
        g_ring_buf[g_ring_head] = val;
        g_ring_head = next;
    }
    /* If full, silently drop — bounded lossy behavior. */
}

/** Pop one byte from ring buffer (called from consumer). */
static inline uint8_t ring_pop(void) {
    if (g_ring_head == g_ring_tail) return 0;
    uint8_t val = g_ring_buf[g_ring_tail];
    g_ring_tail = (g_ring_tail + 1) & PS2_RING_MASK;
    return val;
}

/* =========================================================================
 * Serial Input Polling
 *
 * Polls the serial port (COM1) for incoming bytes and feeds them into
 * the keyboard ring buffer.  This provides keyboard input when PS/2
 * hardware interrupts are not working (e.g. QEMU on Windows).
 *
 * Supports terminal escape sequences for arrow keys:
 *   ESC [ A  = Up     ESC [ B  = Down
 *   ESC [ C  = Right  ESC [ D  = Left
 *   ESC [ H  = Home   ESC [ F  = End
 *   ESC [ 2~ = Insert ESC [ 3~ = Delete
 *   ESC [ 5~ = PageUp ESC [ 6~ = PageDown
 * ========================================================================= */

/** Serial input parser states. */
enum ser_input_state {
    SER_STATE_GROUND,   /**< Normal: next byte is a literal character. */
    SER_STATE_ESC,      /**< Received ESC (0x1B), waiting for '['. */
    SER_STATE_CSI,      /**< Received ESC[, waiting for final char or '~'. */
    SER_STATE_CSI_NUM,  /**< Received ESC[<digit>, collecting number before '~'. */
};

/** Current parser state (persistent across polls). */
static enum ser_input_state g_ser_state = SER_STATE_GROUND;

/** Numeric parameter collected during CSI sequence (e.g. the '2' in ESC[2~). */
static uint8_t g_ser_param = 0;

/**
 * @brief Poll serial port for input and feed into the keyboard ring buffer.
 *
 * Handles terminal escape sequences and translates them to PS/2 key codes.
 * Called from ps2_get_key_timeout() on every key-read attempt.
 */
static void serial_poll_input(void) {
    if (!g_ps2_serial || !g_ps2_serial->ready) return;

    while (serial_data_available(g_ps2_serial)) {
        char c = (char)inb(g_ps2_serial->base);

        switch (g_ser_state) {
        case SER_STATE_GROUND:
            if (c == 0x1B) {            /* ESC */
                g_ser_state = SER_STATE_ESC;
            } else if (c == 0x7F) {     /* DEL → Backspace */
                ring_push(PS2_KEY_BACKSPACE);
            } else if (c == 0x0D) {     /* CR → Enter */
                ring_push(PS2_KEY_ENTER);
            } else if (c == 0x0A) {     /* LF → skip (CR already handled) */
                /* nothing */
            } else if (c == 0x09) {     /* TAB */
                ring_push(PS2_KEY_TAB);
            } else if (c >= 0x20 && c <= 0x7E) {  /* Printable ASCII */
                ring_push((uint8_t)c);
            }
            /* All other control chars: ignore. */
            break;

        case SER_STATE_ESC:
            if (c == '[') {
                g_ser_state = SER_STATE_CSI;
                g_ser_param = 0;
            } else if (c == 'O') {
                /* ESC O sequences (F1-F4 in some terminals) — ignore for now. */
                g_ser_state = SER_STATE_GROUND;
            } else {
                /* Bare ESC — push ESC key, then re-process 'c'. */
                ring_push(PS2_KEY_ESC);
                g_ser_state = SER_STATE_GROUND;
                /* Re-process this byte. */
                if (c == 0x1B) { g_ser_state = SER_STATE_ESC; }
                else if (c == 0x7F) { ring_push(PS2_KEY_BACKSPACE); }
                else if (c == 0x0D) { ring_push(PS2_KEY_ENTER); }
                else if (c >= 0x20 && c <= 0x7E) { ring_push((uint8_t)c); }
            }
            break;

        case SER_STATE_CSI:
            if (c >= '0' && c <= '9') {
                g_ser_param = (uint8_t)(c - '0');
                g_ser_state = SER_STATE_CSI_NUM;
            } else {
                /* ESC [ <letter> — no numeric parameter. */
                switch (c) {
                    case 'A': ring_push(PS2_KEY_UP);      break;
                    case 'B': ring_push(PS2_KEY_DOWN);    break;
                    case 'C': ring_push(PS2_KEY_RIGHT);   break;
                    case 'D': ring_push(PS2_KEY_LEFT);    break;
                    case 'H': ring_push(PS2_KEY_HOME);    break;
                    case 'F': ring_push(PS2_KEY_END);     break;
                    default: break; /* Ignore unknown sequences. */
                }
                g_ser_state = SER_STATE_GROUND;
            }
            break;

        case SER_STATE_CSI_NUM:
            if (c >= '0' && c <= '9') {
                g_ser_param = g_ser_param * 10 + (uint8_t)(c - '0');
            } else if (c == '~') {
                /* ESC[<num>~ — function/extended keys. */
                switch (g_ser_param) {
                    case 1:  ring_push(PS2_KEY_HOME);      break;
                    case 2:  ring_push(PS2_KEY_INSERT);    break;
                    case 3:  ring_push(PS2_KEY_DELETE);    break;
                    case 4:  ring_push(PS2_KEY_END);       break;
                    case 5:  ring_push(PS2_KEY_PAGE_UP);   break;
                    case 6:  ring_push(PS2_KEY_PAGE_DOWN); break;
                    case 11: ring_push(PS2_KEY_F1);        break;
                    case 12: ring_push(PS2_KEY_F2);        break;
                    case 13: ring_push(PS2_KEY_F3);        break;
                    case 14: ring_push(PS2_KEY_F4);        break;
                    case 15: ring_push(PS2_KEY_F5);        break;
                    case 17: ring_push(PS2_KEY_F6);        break;
                    case 18: ring_push(PS2_KEY_F7);        break;
                    case 19: ring_push(PS2_KEY_F8);        break;
                    case 20: ring_push(PS2_KEY_F9);        break;
                    case 21: ring_push(PS2_KEY_F10);       break;
                    case 23: ring_push(PS2_KEY_F11);       break;
                    case 24: ring_push(PS2_KEY_F12);       break;
                    default: break;
                }
                g_ser_state = SER_STATE_GROUND;
            } else {
                /* Unexpected byte after number — abort sequence. */
                g_ser_state = SER_STATE_GROUND;
            }
            break;
        }
    }
}

/* =========================================================================
 * Scancode Translation
 * ========================================================================= */

/**
 * @brief Translate a Scancode Set 1 make code to ASCII.
 *
 * Handles Shift and CapsLock modifier interaction:
 *   - Letter keys (a-z, A-Z): toggled by CapsLock, overridden by Shift
 *   - Symbol keys (1-9, etc.): only affected by Shift
 *
 * @param scancode  Raw make code (0x01-0x57).
 * @return ASCII character, or 0 if no mapping.
 */
static uint8_t scancode_to_ascii(uint8_t scancode) {
    if (scancode >= 128) return 0;

    bool shift = (g_modifiers & PS2_MOD_SHIFT) != 0;
    bool ctrl  = (g_modifiers & PS2_MOD_CTRL) != 0;
    bool caps  = (g_modifiers & PS2_MOD_CAPSLOCK) != 0;

    /* Letters (scancodes 0x10-0x19 = q-p, 0x1E-0x26 = a-l, 0x2C-0x32 = z-m). */
    bool is_letter = (scancode >= 0x10 && scancode <= 0x19) ||
                     (scancode >= 0x1E && scancode <= 0x26) ||
                     (scancode >= 0x2C && scancode <= 0x32);

    if (is_letter) {
        bool uppercase = shift ^ caps;
        uint8_t ch = uppercase ? sc1_shifted[scancode] : sc1_unshifted[scancode];
        /* Ctrl+letter → control character (Ctrl+A=0x01 .. Ctrl+Z=0x1A). */
        if (ctrl) return ch & 0x1F;
        return ch;
    }

    /* Non-letter keys: Shift selects shifted table. */
    return shift ? sc1_shifted[scancode] : sc1_unshifted[scancode];
}

/* =========================================================================
 * IRQ 1 Handler (Keyboard Interrupt)
 * ========================================================================= */

/**
 * @brief Process a raw PS/2 scancode through the state machine.
 *
 * Shared between the IRQ handler (interrupt-driven) and the polling
 * fallback (polls OBF directly). This avoids duplicating the
 * scancode decoding logic.
 *
 * State machine:
 *   1. 0xE0 prefix → set extended flag, wait for next byte
 *   2. Break code (bit 7 set) → update modifier state, clear extended
 *   3. Make code (bit 7 clear) → translate and enqueue character
 *
 * @param scancode  Raw byte from port 0x60.
 */
static void ps2_process_scancode(uint8_t scancode) {
    /* Extended prefix (0xE0) — set flag, wait for next byte. */
    if (scancode == 0xE0) {
        g_extended = true;
        return;
    }

    bool is_break = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (is_break) {
        /* Key release — update modifier state. */
        if (g_extended) {
            /* Extended break codes: right Ctrl/Alt release. */
            if (code < 128) {
                uint8_t mod = ext_scancodes[code];
                if (mod >= PS2_MOD_RCTRL && mod <= PS2_MOD_RALT) {
                    g_modifiers &= ~mod;
                }
            }
        } else {
            /* Standard break codes: modifier key releases. */
            switch (code) {
                case 0x2A: g_modifiers &= ~PS2_MOD_LSHIFT; break;
                case 0x36: g_modifiers &= ~PS2_MOD_RSHIFT; break;
                case 0x1D: g_modifiers &= ~PS2_MOD_LCTRL;  break;
                case 0x38: g_modifiers &= ~PS2_MOD_LALT;   break;
            }
        }
        g_extended = false;
        return;
    }

    /* Make code (key press). */
    uint8_t decoded = 0;

    if (g_extended) {
        /* Extended make codes: arrow keys, F-keys, right modifiers. */
        if (code < 128) {
            uint8_t special = ext_scancodes[code];
            if (special >= PS2_KEY_UP && special <= PS2_KEY_DELETE) {
                decoded = special;
            } else if (special >= PS2_MOD_RCTRL && special <= PS2_MOD_RALT) {
                g_modifiers |= special;
            }
        }
    } else {
        /* Standard make codes: update modifier state and decode character. */
        switch (code) {
            case 0x2A: g_modifiers |= PS2_MOD_LSHIFT;    return;
            case 0x36: g_modifiers |= PS2_MOD_RSHIFT;    return;
            case 0x1D: g_modifiers |= PS2_MOD_LCTRL;     return;
            case 0x38: g_modifiers |= PS2_MOD_LALT;      return;
            case 0x3A: /* CapsLock toggle. */
                g_modifiers ^= PS2_MOD_CAPSLOCK;
                return;
            default:
                decoded = scancode_to_ascii(code);
                break;
        }
    }

    if (decoded != 0) {
        ring_push(decoded);
    }

    g_extended = false;
}

/**
 * @brief IRQ 1 interrupt handler — called on every key press/release.
 *
 * @param frame  Interrupt frame (unused).
 */
static void ps2_irq_handler(interrupt_frame_t *frame) {
    (void)frame;
    g_irq_count++;
    uint8_t scancode = inb(PS2_DATA_PORT);
    ps2_process_scancode(scancode);
    pic_send_eoi(PS2_IRQ_LINE);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/** Write a hex byte to serial. */
static void ser_hex8(uint8_t v) {
    if (!g_ps2_serial) return;
    static const char h[] = "0123456789ABCDEF";
    char buf[3] = { h[(v>>4)&0xF], h[v&0xF], 0 };
    serial_write_string(g_ps2_serial, buf);
}

/** Log a string to serial (NULL-safe). */
static void ser_log(const char *s) {
    if (g_ps2_serial) serial_write_string(g_ps2_serial, s);
}

/**
 * Wait for OBF (Output Buffer Full) on the PS/2 controller.
 * When set, a byte is ready to read from port 0x60.
 *
 * @param timeout  Max iterations to poll.
 * @return true if OBF set, false on timeout.
 */
static bool wait_obf(int timeout) {
    for (int i = 0; i < timeout; i++) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) return true;
    }
    return false;
}

/**
 * Wait for IBF (Input Buffer Full) to clear on the PS/2 controller.
 * When clear, the controller is ready to accept a new byte on port 0x60/0x64.
 *
 * @param timeout  Max iterations to poll.
 * @return true if IBF clear, false on timeout.
 */
static bool wait_ibf(int timeout) {
    for (int i = 0; i < timeout; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_IBF)) return true;
    }
    return false;
}

void ps2_init(void *serial_dev) {
    g_ps2_serial = (serial_dev_t *)serial_dev;

    ser_log("[PS2] Initializing PS/2 keyboard...\r\n");

    /* =================================================================
     * Phase 1: Controller-level reset and self-test
     *
     * We must verify the i8042 controller itself is functional before
     * talking to the keyboard device. This catches broken controllers
     * early and puts the hardware in a known-good state.
     * ================================================================= */

    /* Flush output buffer — read and discard any pending data. */
    (void)inb(PS2_DATA_PORT);

    /* Disable keyboard and mouse at controller level. */
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_STATUS_PORT, PS2_CMD_DISABLE_KB);  /* 0xAD */
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_STATUS_PORT, 0xA7);                /* Disable auxiliary (mouse). */

    /* Flush output buffer again after disabling. */
    wait_obf(PS2_TIMEOUT);
    (void)inb(PS2_DATA_PORT);

    /* Controller self-test (0xAA → port 0x64).
     * Expected response: 0x55 in output buffer. */
    ser_log("[PS2] Controller self-test (0xAA)...");
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_STATUS_PORT, 0xAA);

    if (wait_obf(PS2_TIMEOUT)) {
        uint8_t resp = inb(PS2_DATA_PORT);
        ser_log(" 0x");
        ser_hex8(resp);
        ser_log(resp == 0x55 ? " (OK)\r\n" : " (UNEXPECTED)\r\n");
    } else {
        ser_log(" TIMEOUT (controller not responding!)\r\n");
    }

    /* Test keyboard port (0xAB → port 0x64).
     * Expected response: 0x00 = no error. */
    ser_log("[PS2] Keyboard port test (0xAB)...");
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_STATUS_PORT, 0xAB);

    if (wait_obf(PS2_TIMEOUT)) {
        uint8_t resp = inb(PS2_DATA_PORT);
        ser_log(" 0x");
        ser_hex8(resp);
        ser_log(resp == 0x00 ? " (OK)\r\n" : " (error)\r\n");
    } else {
        ser_log(" TIMEOUT\r\n");
    }

    /* =================================================================
     * Phase 2: Controller configuration
     *
     * Read the controller configuration byte, ensure IRQs and
     * translation are disabled, then write it back.
     * ================================================================= */

    /* Read command byte (0x20 → port 0x64; response in port 0x60). */
    ser_log("[PS2] Read config byte (0x20)...");
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_STATUS_PORT, 0x20);

    uint8_t config = 0;
    if (wait_obf(PS2_TIMEOUT)) {
        config = inb(PS2_DATA_PORT);
        ser_log(" 0x");
        ser_hex8(config);
        ser_log("\r\n");
    } else {
        ser_log(" TIMEOUT\r\n");
    }

    /* PS/2 controller config byte bits:
     *   bit 0: IRQ1 (keyboard interrupt) — will re-enable after handler registered
     *   bit 1: IRQ12 (mouse interrupt) — keep disabled
     *   bit 4: Keyboard disable — MUST clear or keyboard clock line stays low,
     *          preventing the keyboard from sending scancodes
     *   bit 5: Mouse disable — keep disabled
     *   bit 6: Scancode translation — SET (enables controller to translate
     *          default Set 2 scancodes to Set 1, matching our decode tables) */
    config &= ~(1 << 0);  /* Disable IRQ1 — re-enable later after handler registered. */
    config &= ~(1 << 1);  /* Disable IRQ12 (mouse). */
    config &= ~(1 << 4);  /* Enable keyboard (clear disable bit!). */
    config &= ~(1 << 5);  /* Enable mouse (clear disable bit). */
    config &= ~(1 << 6);  /* Disable scancode translation — we handle Set 1 at keyboard, not controller. */

    /* Write command byte (0x60 → port 0x64, then config → port 0x60). */
    ser_log("[PS2] Write config byte (0x60): 0x");
    ser_hex8(config);
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_STATUS_PORT, 0x60);
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_DATA_PORT, config);
    ser_log("\r\n");

    /* =================================================================
     * Phase 3: Enable keyboard interface and reset device
     *
     * Now that the controller is configured, re-enable the keyboard
     * port and reset the keyboard device itself.
     * ================================================================= */

    /* Enable keyboard (0xAE → port 0x64). */
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_STATUS_PORT, PS2_CMD_ENABLE_KB);  /* 0xAE */

    /* Flush any stale output data. */
    wait_obf(PS2_TIMEOUT);
    (void)inb(PS2_DATA_PORT);

    /* Reset keyboard device (0xFF → port 0x60).
     * Keyboard responds with ACK (0xFA) then self-test result (0xAA). */
    ser_log("[PS2] Keyboard reset (0xFF)...");
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_DATA_PORT, PS2_CMD_RESET);

    /* Read ACK. */
    if (wait_obf(PS2_TIMEOUT)) {
        uint8_t ack = inb(PS2_DATA_PORT);
        ser_log(" ACK=0x");
        ser_hex8(ack);
    } else {
        ser_log(" no ACK");
    }

    /* Read self-test result. */
    if (wait_obf(PS2_TIMEOUT)) {
        uint8_t st = inb(PS2_DATA_PORT);
        ser_log(" self-test=0x");
        ser_hex8(st);
        ser_log(st == 0xAA ? " (OK)\r\n" : " (FAIL)\r\n");
    } else {
        ser_log(" no self-test\r\n");
    }

    /* =================================================================
     * Phase 4: Select scancode set 1
     *
     * After reset the keyboard defaults to Set 2. Our decode tables
     * (sc1_unshifted, sc1_shifted, sc1_ctrl, sc1_alt) expect Set 1
     * scancodes, so we must command the keyboard to switch.
     *
     * Command sequence (0xF0 0x01):
     *   0xF0  — Set/Get scancode set
     *   0x01  — Select Set 1 (PC/XT compatible)
     * The keyboard ACKs (0xFA) after each byte.
     * ================================================================= */

    ser_log("[PS2] Select scancode set 1 (0xF0 0x01)...");
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_DATA_PORT, 0xF0);

    if (wait_obf(PS2_TIMEOUT)) {
        uint8_t ack = inb(PS2_DATA_PORT);
        ser_log(" ACK1=0x");
        ser_hex8(ack);
    } else {
        ser_log(" ACK1=TIMEOUT");
    }

    wait_ibf(PS2_TIMEOUT);
    outb(PS2_DATA_PORT, 0x01);

    if (wait_obf(PS2_TIMEOUT)) {
        uint8_t ack = inb(PS2_DATA_PORT);
        ser_log(" ACK2=0x");
        ser_hex8(ack);
        ser_log(ack == 0xFA ? " (OK)\r\n" : "\r\n");
    } else {
        ser_log(" ACK2=TIMEOUT\r\n");
    }

    /* =================================================================
     * Phase 5: Enable scanning
     *
     * Send 0xF4 (enable scanning) to the keyboard. The keyboard
     * should ACK with 0xFA. After this, keypresses generate
     * scancodes that appear in the output buffer.
     * ================================================================= */

    ser_log("[PS2] Enable scanning (0xF4)...");
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_DATA_PORT, PS2_CMD_ENABLE_SCAN);

    if (wait_obf(PS2_TIMEOUT)) {
        uint8_t ack = inb(PS2_DATA_PORT);
        ser_log(" ACK=0x");
        ser_hex8(ack);
        ser_log(ack == 0xFA ? " (OK)\r\n" : "\r\n");
    } else {
        ser_log(" TIMEOUT\r\n");
    }

    /* =================================================================
     * Phase 6: Register IRQ handler and enable interrupts
     *
     * Now that the keyboard is fully initialized and scanning,
     * register our IRQ 1 handler and unmask the interrupt line.
     * We kept IRQs disabled during init to avoid spurious interrupts
     * from the initialization sequence itself.
     * ================================================================= */

    isr_register_handler(PS2_IRQ_VECTOR, ps2_irq_handler);

    /* Re-enable IRQ1 in the PIC. */
    pic_unmask_irq(PS2_IRQ_LINE);

    /* Also re-enable IRQ1 in the controller config byte (set bit 0). */
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_STATUS_PORT, 0x60);
    wait_ibf(PS2_TIMEOUT);
    outb(PS2_DATA_PORT, config | (1 << 0));

    g_initialized = true;

    /* =================================================================
     * Phase 6: Final diagnostics
     * ================================================================= */

    ser_log("[PS2] Initialized. IRQ1 unmasked.");

    /* Dump PIC IMR. */
    uint8_t master_imr = pic_get_imr_master();
    ser_log(" PIC IMR=0x");
    ser_hex8(master_imr);
    ser_log((master_imr & 0x02) ? " (IRQ1 MASKED!!)" : " (OK)");
    ser_log("\r\n");

    /* Check for stale OBF data. */
    if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) {
        uint8_t stale = inb(PS2_DATA_PORT);
        ser_log("[PS2] Stale OBF data: 0x");
        ser_hex8(stale);
        ser_log("\r\n");
    } else {
        ser_log("[PS2] Output buffer clean.\r\n");
    }

    /* Quick poll: does OBF fire immediately? (keyboard may already have data) */
    ser_log("[PS2] Quick OBF poll (10ms)...");
    for (volatile int d = 0; d < 200000; d++) asm volatile("pause");
    if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) {
        uint8_t data = inb(PS2_DATA_PORT);
        ser_log(" HIT! data=0x");
        ser_hex8(data);
        ser_log("\r\n");
        /* Feed through scancode machine. */
        ps2_process_scancode(data);
    } else {
        ser_log(" nothing.\r\n");
    }
}

uint8_t ps2_get_key(void) {
    return ps2_get_key_timeout(1000);
}

bool ps2_key_available(void) {
    return ring_count() > 0;
}

uint8_t ps2_get_modifiers(void) {
    return g_modifiers;
}

uint8_t ps2_get_key_timeout(uint32_t timeout_ms) {
    if (!g_initialized) return 0;

    /* --- Poll serial port for input (backup if PS/2 IRQ doesn't work). */
    serial_poll_input();

    /* --- Fast path: check ring buffer first. */
    if (ring_count() > 0) return ring_pop();

    /* --- Polling fallback: read PS/2 controller directly --- */

    uint64_t deadline;
    if (timer_is_initialized()) {
        deadline = timer_get_ticks() + ((uint64_t)timeout_ms * PIT_DEFAULT_FREQ / 1000);
    } else {
        deadline = 0;
    }

    do {
        /* Also poll serial input every iteration. */
        serial_poll_input();
        if (ring_count() > 0) return ring_pop();

        /* Poll the OBF bit (bit 0 of status port 0x64). */
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) {
            g_obf_count++;
            uint8_t raw = inb(PS2_DATA_PORT);
            ps2_process_scancode(raw);

            if (ring_count() > 0) {
                return ring_pop();
            }
        }

        /* Yield CPU until next timer tick. */
        uint64_t rflags;
        asm volatile ("pushfq; pop %0" : "=r"(rflags));
        if ((rflags & 0x200) && timer_is_initialized()) {
            if (timer_get_ticks() >= deadline) return 0;
            asm volatile ("hlt");
        } else {
            for (volatile int i = 0; i < 50000; i++) { asm volatile ("pause"); }
            return (ring_count() > 0) ? ring_pop() : 0;
        }
    } while (1);
}

uint32_t ps2_get_irq_count(void) {
    return g_irq_count;
}

uint32_t ps2_get_obf_count(void) {
    return g_obf_count;
}
