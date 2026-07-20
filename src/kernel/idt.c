/**
 * @file idt.c
 * @brief Interrupt Descriptor Table initialization and gate descriptor
 *        encoding.
 *
 * The IDT is statically allocated and populated at boot time.  Each of
 * the 256 gate descriptors points to the corresponding assembly stub
 * in isr_stubs[vector], using the kernel code segment selector (0x08)
 * and an interrupt gate type (0xE) which clears IF on entry.
 *
 * Vector 2 (NMI) is special-cased to use a trap gate (type 0xF) so that
 * NMI delivery is not blocked by the IF flag.
 *
 * The double-fault handler (vector 8) is special-cased to use IST index 1,
 * which causes the CPU to unconditionally switch to a dedicated 4 KiB
 * emergency stack before executing the handler.  This prevents a
 * triple-fault when the kernel stack itself is corrupted.
 */

#include "idt.h"
#include "isr.h"
#include "../kernel/gdt.h"
#include <stdint.h>

/* =========================================================================
 * Static storage
 * ========================================================================= */

/** 256 IDT gate descriptors, 16 bytes each, aligned to 16 for cache. */
static idt_entry_t idt_entries[IDT_ENTRIES]
    __attribute__((section(".data"), aligned(16)));

/** IDTR pseudo-descriptor loaded by lidt. */
static idt_pointer_t idt_ptr;

/** 4 KiB emergency stack for the double-fault handler (IST1). */
static uint8_t ist1_stack[4096]
    __attribute__((section(".bss"), aligned(16)));

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * @brief Encode one IDT gate descriptor.
 *
 * Populates all fields of idt_entries[vector] from the given parameters.
 * The reserved field is always set to zero (hardware requirement).
 *
 * @param vector       IDT entry index (0-255).
 * @param handler_addr Linear address of the assembly stub.
 * @param selector     Code segment selector (kernel CS = 0x08).
 * @param ist          IST index (0 = use normal RSP0 stack, 1-7 = IST).
 * @param type_attr    Gate type attributes byte: P | DPL | 0 | Type.
 */
static void idt_set_gate(uint8_t vector, uint64_t handler_addr,
                         uint16_t selector, uint8_t ist, uint8_t type_attr) {
    idt_entries[vector].offset_low  = (uint16_t)(handler_addr & 0xFFFF);
    idt_entries[vector].selector    = selector;
    idt_entries[vector].ist         = ist & 0x07;
    idt_entries[vector].type_attr   = type_attr;
    idt_entries[vector].offset_mid  = (uint16_t)((handler_addr >> 16) & 0xFFFF);
    idt_entries[vector].offset_high = (uint32_t)((handler_addr >> 32) & 0xFFFFFFFF);
    idt_entries[vector].reserved    = 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void idt_init(void) {
    /* ------------------------------------------------------------------
     * 1. Zero all 256 IDT entries.
     * ------------------------------------------------------------------ */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_entries[i].offset_low  = 0;
        idt_entries[i].selector    = 0;
        idt_entries[i].ist         = 0;
        idt_entries[i].type_attr   = 0;
        idt_entries[i].offset_mid  = 0;
        idt_entries[i].offset_high = 0;
        idt_entries[i].reserved    = 0;
    }

    /* ------------------------------------------------------------------
     * 2. Populate gate descriptors for all 256 vectors.
     *
     * Each entry points to the corresponding assembly stub in
     * isr_stub_table[vector], using the kernel code segment (0x08)
     * and an interrupt gate (type 0xE, present, DPL=0).
     *
     * type_attr = P(1) | DPL(0) | 0 | Type(0xE) = 0x8E
     * ------------------------------------------------------------------ */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate((uint8_t)i,
                     isr_stub_table[i],
                     GDT_KERNEL_CS_SEL,
                     0,      /* IST = 0 (normal stack) */
                     0x8E);  /* interrupt gate, present, DPL=0 */
    }

    /* ------------------------------------------------------------------
     * 3. Override specific vectors for special behaviour.
     *
     * Vector 2 (NMI): use a trap gate (type 0xF) instead of an interrupt
     * gate.  NMIs are edge-triggered and must not be masked by IF; using
     * a trap gate ensures the CPU does not automatically clear IF on NMI
     * entry, which would leave the system in a confusing state if the NMI
     * handler needs to return.
     *
     * Vector 8 (Double Fault): use IST index 1 for an emergency stack.
     * When the CPU vectors to the double-fault handler, it unconditionally
     * loads RSP from TSS.IST1 instead of TSS.RSP0.  This ensures the
     * handler runs on a known-good stack even if the current kernel stack
     * is corrupted or exhausted.
     * ------------------------------------------------------------------ */
    idt_set_gate(2,
                 isr_stub_table[2],
                 GDT_KERNEL_CS_SEL,
                 0,      /* IST = 0 (normal stack) */
                 0x8F);  /* trap gate, present, DPL=0 */

    tss_set_ist(0, (uint64_t)(ist1_stack + sizeof(ist1_stack)));
    idt_set_gate(8,
                 isr_stub_table[8],
                 GDT_KERNEL_CS_SEL,
                 1,      /* IST = 1 (double-fault emergency stack) */
                 0x8E);  /* interrupt gate, present, DPL=0 */

    /* ------------------------------------------------------------------
     * 4. Install the IDT via lidt.
     * ------------------------------------------------------------------ */
    idt_ptr.limit = (uint16_t)(sizeof(idt_entries) - 1);
    idt_ptr.base  = (uint64_t)idt_entries;
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}

uint64_t idt_get_address(void) {
    return (uint64_t)idt_entries;
}
