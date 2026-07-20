/**
 * @file isr.c
 * @brief ISR handler dispatch table and common interrupt dispatcher.
 *
 * The handler table maps each of the 256 interrupt vectors to an optional
 * C handler function.  When an interrupt fires, the assembly stub saves
 * the full CPU state into an interrupt_frame_t and calls isr_common_handler().
 *
 * isr_common_handler() performs three tasks:
 *   1. Dispatches to the registered per-vector handler (if any).
 *   2. Sends End-Of-Interrupt (EOI) to the PIC for hardware IRQs
 *      (vectors 32-47), preventing the PIC from re-asserting the IRQ.
 *   3. Returns to the assembly stub which restores registers and iretqs.
 *
 * EOI is sent even when no user handler is registered — otherwise the
 * PIC's In-Service Register (ISR) bit is never cleared and the PIC
 * will not deliver any further interrupts on that line.
 *
 * Spurious IRQ suppression:
 *   Spurious IRQs (master IRQ 7 → vector 39, slave IRQ 15 → vector 47)
 *   do NOT set a bit in the PIC's ISR register.  Sending a non-specific
 *   EOI in that case would clear whatever higher-priority ISR bit happens
 *   to be set, corrupting the PIC state.  For this reason:
 *     - Vector 39 (master spurious):  EOI is suppressed entirely.
 *     - Vector 47 (slave spurious):   EOI is sent to the master only
 *       (to clear the cascade IRQ 2 ISR bit, which IS set), but not
 *       to the slave (its ISR bit was never set).
 */

#include "isr.h"
#include "pic.h"

/* =========================================================================
 * Handler dispatch table
 *
 * One function pointer per vector, indexed by vector number.  Initialized
 * to NULL (all vectors unhandled).  isr_register_handler() sets the entry;
 * isr_unregister_handler() clears it.
 * ========================================================================= */

static isr_handler_fn handlers[256];

/* =========================================================================
 * Spurious IRQ vector constants (after PIC remap)
 *
 * Master IRQ 7 → vector 32 + 7 = 39
 * Slave  IRQ 15 → vector 40 + 7 = 47
 *
 * Spurious IRQs do not set the PIC's ISR bit, so sending a non-specific
 * EOI would corrupt the highest-priority in-service bit.
 * ========================================================================= */
#define SPURIOUS_MASTER_VEC  39
#define SPURIOUS_SLAVE_VEC   47

/* =========================================================================
 * Public API
 * ========================================================================= */

void isr_init(void) {
    for (int i = 0; i < 256; i++) {
        handlers[i] = (isr_handler_fn)0;
    }
}

void isr_register_handler(uint8_t vector, isr_handler_fn handler) {
    handlers[vector] = handler;
}

void isr_unregister_handler(uint8_t vector) {
    handlers[vector] = (isr_handler_fn)0;
}

/* =========================================================================
 * Common dispatcher — called from every assembly stub
 * ========================================================================= */

void isr_common_handler(interrupt_frame_t *frame) {
    uint8_t vector = (uint8_t)frame->vector;

    /* Dispatch to the registered handler, if any. */
    if (handlers[vector]) {
        handlers[vector](frame);
    }

    /* Hardware IRQs (vectors 32-47, remapped from PIC IRQ 0-15)
     * MUST receive an EOI or the PIC will not deliver further
     * interrupts on that line.
     *
     * Exception: spurious IRQs (vectors 39 and 47) must NOT receive
     * a full EOI — see file-level comment for the rationale. */
    if (vector >= 32 && vector <= 47) {
        if (vector == SPURIOUS_MASTER_VEC) {
            /* Master spurious (IRQ 7): no ISR bit was set on either PIC.
             * Sending EOI would clear the wrong ISR bit — suppress it. */
        } else if (vector == SPURIOUS_SLAVE_VEC) {
            /* Slave spurious (IRQ 15): the slave's ISR bit was not set,
             * but the master's cascade line (IRQ 2) ISR bit WAS set.
             * Send EOI to master only to clear the cascade bit. */
            pic_send_eoi_master_only();
        } else {
            pic_send_eoi((uint8_t)(vector - 32));
        }
    }
}
