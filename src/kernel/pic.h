/**
 * @file pic.h
 * @brief 8259A Programmable Interrupt Controller (PIC) driver interface.
 *
 * The x86 PC/AT architecture uses two cascaded 8259A PICs:
 *
 *   PIC1 (Master)                PIC2 (Slave)
 *   I/O ports: 0x20 (cmd)        I/O ports: 0xA0 (cmd)
 *              0x21 (data)                   0xA1 (data)
 *   IRQ 0-7 (INT 32-39)          IRQ 8-15 (INT 40-47)
 *   Connected to CPU INT pin 0    Cascaded through PIC1 IRQ 2
 *
 * After ICW initialization, IRQ lines are remapped so that:
 *   Master IRQ 0-7  → INT 32-39  (avoids collision with CPU exceptions 0-31)
 *   Slave  IRQ 8-15 → INT 40-47
 *
 * All IRQs are masked (disabled) after init.  Individual IRQ lines are
 * enabled via pic_unmask_irq() when their handler is ready.
 *
 * References:
 *   Intel 8259A Programmable Interrupt Controller datasheet
 *   OSDev wiki — 8259 PIC
 *   OSDev wiki — Interrupts
 */

#ifndef KERNEL_PIC_H
#define KERNEL_PIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * I/O port addresses
 * ========================================================================= */

/** Master PIC command/port register. */
#define PIC1_CMD_PORT   0x20
/** Master PIC data register. */
#define PIC1_DATA_PORT  0x21

/** Slave PIC command/port register. */
#define PIC2_CMD_PORT   0xA0
/** Slave PIC data register. */
#define PIC2_DATA_PORT  0xA1

/* =========================================================================
 * ICW (Initialization Command Word) constants
 * ========================================================================= */

/** ICW1: begin INIT sequence + ICW4 required. */
#define PIC_ICW1_INIT       0x11

/** ICW4: 8086/8088 mode (required for x86-64). */
#define PIC_ICW4_8086       0x01

/** Non-specific EOI command — clears the highest-priority ISR bit. */
#define PIC_EOI             0x20

/* =========================================================================
 * Remap vector offsets
 *
 * After initialization, master IRQ 0-7 map to INT 32-39 and slave
 * IRQ 8-15 map to INT 40-47.  These offsets are programmed via ICW2.
 * ========================================================================= */

#define PIC1_VECTOR_OFFSET  0x20    /* IRQ 0 → INT 0x20 (32) */
#define PIC2_VECTOR_OFFSET  0x28    /* IRQ 8 → INT 0x28 (40) */

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize both PICs: remap IRQ vectors, then mask all IRQ lines.
 *
 * Performs the ICW1-ICW4 initialization sequence on both master and slave
 * PICs, remapping IRQ 0-7 to INT 32-39 and IRQ 8-15 to INT 40-47.
 * After initialization, all 15 IRQ lines are masked (disabled).
 *
 * Must be called before enabling interrupts (sti) and before any
 * IRQ-specific unmask call.
 */
void pic_init(void);

/**
 * @brief Mask (disable) a specific IRQ line.
 *
 * Sets the corresponding bit in the IMR (Interrupt Mask Register)
 * of the PIC that owns the given IRQ line.
 *
 * @param irq  IRQ line number (0-15).  0-7 = master, 8-15 = slave.
 */
void pic_mask_irq(uint8_t irq);

/**
 * @brief Unmask (enable) a specific IRQ line.
 *
 * Clears the corresponding bit in the IMR of the PIC that owns the
 * given IRQ line.  For slave IRQs (8-15), also unmasks IRQ 2 on the
 * master PIC (the cascade line) if not already unmasked.
 *
 * @param irq  IRQ line number (0-15).  0-7 = master, 8-15 = slave.
 */
void pic_unmask_irq(uint8_t irq);

/**
 * @brief Send End-Of-Interrupt (EOI) to the appropriate PIC(s).
 *
 * Must be called at the end of every hardware IRQ handler to tell the
 * PIC that the interrupt has been serviced and it may assert the next
 * pending interrupt.
 *
 * For master IRQs (0-7): sends EOI to master PIC only.
 * For slave IRQs (8-15): sends EOI to slave PIC, then to master PIC
 * (because the slave is cascaded through master IRQ 2).
 *
 * @param irq  IRQ line number (0-15).
 */
void pic_send_eoi(uint8_t irq);

/**
 * @brief Send EOI to the master PIC only (skips the slave).
 *
 * Used for spurious slave IRQs (IRQ 15) where the slave's ISR bit was
 * never set (so sending EOI to the slave would be a no-op at best and
 * could clear a legitimate ISR bit at worst), but the master's cascade
 * line (IRQ 2) ISR bit must be cleared.
 */
void pic_send_eoi_master_only(void);

/**
 * @brief Read the Interrupt Mask Register (IMR) of the master PIC.
 *
 * The IMR is an 8-bit register where each set bit means the
 * corresponding IRQ line is masked (disabled).
 *
 * @return Current master IMR value.
 */
uint8_t pic_get_imr_master(void);

/**
 * @brief Read the Interrupt Mask Register (IMR) of the slave PIC.
 *
 * @return Current slave IMR value.
 */
uint8_t pic_get_imr_slave(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_PIC_H */
