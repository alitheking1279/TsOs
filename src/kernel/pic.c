/**
 * @file pic.c
 * @brief 8259A PIC driver implementation.
 *
 * Initialization sequence (per Intel 8259A datasheet, ICW1-ICW4):
 *
 *   1. ICW1 (INIT + ICW4 required) → command port
 *   2. ICW2 (vector offset)         → data port
 *   3. ICW3 (cascade configuration) → data port
 *   4. ICW4 (8086 mode)             → data port
 *
 * After ICW4, all IRQ lines are masked by writing 0xFF to each data port.
 * Individual IRQs are enabled later via pic_unmask_irq().
 *
 * The EOI (End-Of-Interrupt) must be sent to the PIC that raised the
 * interrupt.  For slave IRQs (8-15), both slave and master must receive
 * EOI because the slave is cascaded through master IRQ 2.
 */

#include "pic.h"

/* ---- Low-level port I/O ------------------------------------------------
 * Defined locally to avoid coupling with the serial driver, which defines
 * its own identical static inline versions.  In a larger kernel these
 * would live in a shared <portio.h>. */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/** Delay required by the 8259A datasheet between consecutive ICW writes.
 *  Writing to port 0x80 (BIOS POST diagnostic port) burns ~1 microsecond
 *  on real hardware.  Harmless on QEMU but critical for bare-metal. */
static inline void pic_io_delay(void) {
    outb(0x80, 0);
}

/* ---- Helper: determine which PIC owns an IRQ line --------------------- */

/** Return 1 if the IRQ line belongs to the slave PIC. */
static int is_slave_irq(uint8_t irq) {
    return irq >= 8;
}

/** Return the IRQ line number relative to its PIC (0-7). */
static uint8_t local_irq(uint8_t irq) {
    return irq - (is_slave_irq(irq) ? 8 : 0);
}

/* ---- Helper: read/write the IMR for a given PIC ----------------------- */

/** Read the IMR from the specified PIC's data port. */
static uint8_t pic_read_imr(uint16_t data_port) {
    return inb(data_port);
}

/** Write a new IMR value to the specified PIC's data port. */
static void pic_write_imr(uint16_t data_port, uint8_t imr) {
    outb(data_port, imr);
}

/* ---- Public API --------------------------------------------------------*/

void pic_init(void) {
    /* ---------------------------------------------------------------
     * Master PIC (ports 0x20/0x21):
     *   ICW1: INIT=1, ICW4=1 → 0x11
     *   ICW2: vector offset = 0x20 (IRQ 0 → INT 32)
     *   ICW3: slave connected on IRQ 2 (bit 2 set) → 0x04
     *   ICW4: 8086 mode → 0x01
     *
     * Each ICW write must be followed by a ~1 µs delay (per the 8259A
     * datasheet) before the next write is accepted.  Writing to port
     * 0x80 (POST diagnostic) burns approximately that long on real
     * hardware.
     * --------------------------------------------------------------- */
    outb(PIC1_CMD_PORT,  PIC_ICW1_INIT);
    pic_io_delay();
    outb(PIC1_DATA_PORT, PIC1_VECTOR_OFFSET);
    pic_io_delay();
    outb(PIC1_DATA_PORT, 0x04);              /* slave on IRQ2 */
    pic_io_delay();
    outb(PIC1_DATA_PORT, PIC_ICW4_8086);

    /* ---------------------------------------------------------------
     * Slave PIC (ports 0xA0/0xA1):
     *   ICW1: INIT=1, ICW4=1 → 0x11
     *   ICW2: vector offset = 0x28 (IRQ 8 → INT 40)
     *   ICW3: cascade identity = 2 (master expects us on IRQ 2) → 0x02
     *   ICW4: 8086 mode → 0x01
     * --------------------------------------------------------------- */
    outb(PIC2_CMD_PORT,  PIC_ICW1_INIT);
    pic_io_delay();
    outb(PIC2_DATA_PORT, PIC2_VECTOR_OFFSET);
    pic_io_delay();
    outb(PIC2_DATA_PORT, 0x02);              /* cascade identity */
    pic_io_delay();
    outb(PIC2_DATA_PORT, PIC_ICW4_8086);

    /* ---------------------------------------------------------------
     * Mask all IRQ lines (set every bit in both IMRs).
     * Individual IRQs are unmasked later via pic_unmask_irq().
     * --------------------------------------------------------------- */
    pic_write_imr(PIC1_DATA_PORT, 0xFF);
    pic_write_imr(PIC2_DATA_PORT, 0xFF);
}

void pic_mask_irq(uint8_t irq) {
    uint16_t data_port = is_slave_irq(irq) ? PIC2_DATA_PORT : PIC1_DATA_PORT;
    uint8_t bit = (uint8_t)(1U << local_irq(irq));
    uint8_t imr = pic_read_imr(data_port);
    imr |= bit;
    pic_write_imr(data_port, imr);
}

void pic_unmask_irq(uint8_t irq) {
    uint16_t data_port = is_slave_irq(irq) ? PIC2_DATA_PORT : PIC1_DATA_PORT;
    uint8_t bit = (uint8_t)(1U << local_irq(irq));
    uint8_t imr = pic_read_imr(data_port);
    imr &= (uint8_t)~bit;
    pic_write_imr(data_port, imr);

    /* For slave IRQs (8-15), also ensure the cascade line (IRQ 2)
     * is unmasked on the master PIC, otherwise no slave interrupts
     * can reach the CPU. */
    if (is_slave_irq(irq)) {
        uint8_t master_imr = pic_read_imr(PIC1_DATA_PORT);
        master_imr &= (uint8_t)~(1U << 2);  /* clear IRQ2 bit */
        pic_write_imr(PIC1_DATA_PORT, master_imr);
    }
}

void pic_send_eoi(uint8_t irq) {
    if (is_slave_irq(irq)) {
        /* Slave IRQs cascade through master IRQ 2.
         * Send EOI to slave first, then to master. */
        outb(PIC2_CMD_PORT, PIC_EOI);
    }
    /* Always send EOI to the master PIC — either this IRQ is directly
     * on the master, or the slave's cascade triggers master IRQ 2. */
    outb(PIC1_CMD_PORT, PIC_EOI);
}

void pic_send_eoi_master_only(void) {
    outb(PIC1_CMD_PORT, PIC_EOI);
}

uint8_t pic_get_imr_master(void) {
    return pic_read_imr(PIC1_DATA_PORT);
}

uint8_t pic_get_imr_slave(void) {
    return pic_read_imr(PIC2_DATA_PORT);
}
