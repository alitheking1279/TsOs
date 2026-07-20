/**
 * @file idt.h
 * @brief Interrupt Descriptor Table (IDT) interface.
 *
 * The IDT is an array of 256 gate descriptors (16 bytes each), one per
 * interrupt vector.  Each gate specifies:
 *   - The address of the handler (assembly stub)
 *   - The code segment selector (kernel CS = 0x08)
 *   - An optional IST index for stack switching
 *   - Privilege level (DPL) and gate type (interrupt vs trap)
 *
 * 64-bit IDT gate descriptor layout (Intel SDM Vol.3A §6.10, Figure 6-4):
 *
 *   Byte 0-1:   offset[15:0]       Low 16 bits of handler address
 *   Byte 2-3:   selector           Code segment selector
 *   Byte 4:     IST[2:0] | 0       IST index (bits 2:0), reserved=0
 *   Byte 5:     P | DPL | 0 | type Gate type attributes
 *   Byte 6-7:   offset[31:16]      Bits 16-31 of handler address
 *   Byte 8-11:  offset[63:32]      Bits 32-63 of handler address
 *   Byte 12-15: reserved           Must be zero
 *
 * Gate type values (byte 5, bits 3:0):
 *   0xE = 64-bit interrupt gate  (clears IF on entry)
 *   0xF = 64-bit trap gate       (does NOT clear IF)
 *
 * type_attr byte encoding:
 *   bit 7:   P (Present)
 *   bits 6:5: DPL (Descriptor Privilege Level)
 *   bit 4:   0 (must be zero for interrupt/trap gates)
 *   bits 3:0: Type
 *
 * References:
 *   Intel SDM Vol.3A §6.10  — IDT gate descriptor format
 *   Intel SDM Vol.3A §6.11  — IDTR register and lidt/sidt
 */

#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Number of entries in the IDT (256 vectors for x86-64). */
#define IDT_ENTRIES 256

/** 64-bit interrupt gate: clears IF on entry, prevents nested interrupts. */
#define IDT_GATE_INTERRUPT  0xE

/** 64-bit trap gate: does NOT clear IF on entry. */
#define IDT_GATE_TRAP       0xF

/* =========================================================================
 * 64-bit IDT gate descriptor (16 bytes, packed)
 * ========================================================================= */

typedef struct {
    uint16_t offset_low;    /**< [0:1]  Handler address bits [15:0] */
    uint16_t selector;      /**< [2:3]  Code segment selector (e.g. 0x08) */
    uint8_t  ist;           /**< [4]    IST index (bits 2:0), bits 7:3 = 0 */
    uint8_t  type_attr;     /**< [5]    P | DPL | 0 | Type */
    uint16_t offset_mid;    /**< [6:7]  Handler address bits [31:16] */
    uint32_t offset_high;   /**< [8:11] Handler address bits [63:32] */
    uint32_t reserved;      /**< [12:15] Must be zero */
} __attribute__((packed)) idt_entry_t;

/** Compile-time size check: each IDT entry must be exactly16 bytes. */
typedef char _idt_entry_size[(sizeof(idt_entry_t) == 16) ? 1 : -1];

/* =========================================================================
 * IDTR pseudo-descriptor (10 bytes, packed)
 * ========================================================================= */

typedef struct {
    uint16_t limit;         /**< Byte length of IDT minus 1 */
    uint64_t base;          /**< Linear address of IDT[0] */
} __attribute__((packed)) idt_pointer_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Build and install the complete IDT with 256 gate descriptors.
 *
 * Steps:
 *   1. Zero all 256 IDT entries.
 *   2. For each vector 0-255, encode a gate descriptor pointing to the
 *      corresponding assembly stub in isr_stub_table[vector].
 *   3. Override vector 2 (NMI) to use a trap gate (type 0xF) so NMI
 *      delivery is not blocked by the IF flag.
 *   4. Override vector 8 (double fault) to use IST index 1, providing
 *      a dedicated emergency stack.
 *   5. Install the IDT via lidt.
 *
 * After return, the IDT is live and ready to accept interrupts.
 * Interrupts remain disabled (IF=0) until sti is explicitly executed.
 */
void idt_init(void);

/**
 * @return Linear address of the static IDT entry array.
 *         Used by tests to verify the IDTR base matches.
 */
uint64_t idt_get_address(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_IDT_H */
