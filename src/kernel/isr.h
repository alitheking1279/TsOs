/**
 * @file isr.h
 * @brief Interrupt Service Routine (ISR) framework — types, stub table,
 *        and handler registration.
 *
 * Every CPU exception (vectors 0-31) and hardware IRQ (vectors 32-47,
 * after PIC remap) is routed through an assembly stub in isr_stubs.asm.
 * The stub builds an interrupt_frame_t on the stack and calls the common
 * C dispatcher isr_common_handler().
 *
 * Interrupt frame layout (identical for error-code and no-error-code
 * exceptions — this is a design invariant):
 *
 *   Offset   Field          Pushed by
 *   ------   -----          ---------
 *   +0       r15            isr_common
 *   +8       r14            isr_common
 *   +16      r13            isr_common
 *   +24      r12            isr_common
 *   +32      r11            isr_common
 *   +40      r10            isr_common
 *   +48      r9             isr_common
 *   +56      r8             isr_common
 *   +64      rbp            isr_common
 *   +72      rdi            isr_common
 *   +80      rsi            isr_common
 *   +88      rdx            isr_common
 *   +96      rcx            isr_common
 *   +104     rbx            isr_common
 *   +112     rax            isr_common
 *   +120     vector         stub (interrupt number)
 *   +128     error_code     stub (dummy 0) or CPU (real error code)
 *   +136     rip            CPU
 *   +144     cs             CPU
 *   +152     rflags         CPU
 *   +160     rsp            CPU (only on CPL change)
 *   +168     ss             CPU (only on CPL change)
 *
 * Stack alignment proof:
 *   NOERRCODE path: CPU(40) + stub(16) + GP(128) = 184 bytes
 *   ERRCODE path:   CPU(48) + stub(8)  + GP(128) = 184 bytes
 *   Both = 184 ≡ 8 (mod 16).  With RSP_user ≡ 8 (mod 16) (SysV ABI),
 *   RSP before `call` ≡ 0 (mod 16) → C entry RSP ≡ 8 (mod 16).  ✓
 *
 * References:
 *   Intel SDM Vol.3A §6.10  — IDT gate descriptors
 *   Intel SDM Vol.3A §6.12  — Interrupt processing (stack frame)
 *   Intel SDM Vol.3A §3.4.5 — Segment descriptors (error codes)
 */

#ifndef KERNEL_ISR_H
#define KERNEL_ISR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Interrupt frame (pushed on every interrupt/exception)
 * ========================================================================= */

/**
 * @brief Complete CPU state saved by the ISR stub and the CPU itself.
 *
 * The frame is passed as the sole argument (rdi) to isr_common_handler()
 * and to every registered per-vector handler.  Fields are accessible by
 * name and can be modified by the handler (the CPU restores from the
 * modified frame on iretq).
 *
 * @note The struct is packed to match the exact stack layout produced by
 *       the assembly stubs.  Do NOT reorder fields or add padding.
 */
typedef struct {
    /* Pushed by isr_common (16 GP registers, push order = rax first) */
    uint64_t r15;           /**< [+0]   Saved R15 (pushed last = lowest addr) */
    uint64_t r14;           /**< [+8]   Saved R14 */
    uint64_t r13;           /**< [+16]  Saved R13 */
    uint64_t r12;           /**< [+24]  Saved R12 */
    uint64_t r11;           /**< [+32]  Saved R11 */
    uint64_t r10;           /**< [+40]  Saved R10 */
    uint64_t r9;            /**< [+48]  Saved R9 */
    uint64_t r8;            /**< [+56]  Saved R8 */
    uint64_t rbp;           /**< [+64]  Saved RBP */
    uint64_t rdi;           /**< [+72]  Saved RDI (overwritten with frame ptr) */
    uint64_t rsi;           /**< [+80]  Saved RSI */
    uint64_t rdx;           /**< [+88]  Saved RDX */
    uint64_t rcx;           /**< [+96]  Saved RCX */
    uint64_t rbx;           /**< [+104] Saved RBX */
    uint64_t rax;           /**< [+112] Saved RAX */

    /* Pushed by stub */
    uint64_t vector;        /**< [+120] Interrupt vector number (0-255) */
    uint64_t error_code;    /**< [+128] Real error code or dummy 0 */

    /* Pushed by CPU on interrupt entry */
    uint64_t rip;           /**< [+136] Return address (saved EIP) */
    uint64_t cs;            /**< [+144] Code segment selector */
    uint64_t rflags;        /**< [+152] Saved RFLAGS */
    uint64_t rsp;           /**< [+160] Stack pointer (only on CPL change) */
    uint64_t ss;            /**< [+168] Stack segment (only on CPL change) */
} __attribute__((packed)) interrupt_frame_t;

/** Compile-time assertion: frame must be exactly176 bytes. */
typedef char _iframe_size_check[(sizeof(interrupt_frame_t) == 176) ? 1 : -1];

/* =========================================================================
 * ISR stub table (defined in isr_stubs.asm, section .rodata)
 * ========================================================================= */

/**
 * Array of 256 8-byte pointers, one per vector.  Each entry holds the
 * linear address of the corresponding assembly stub (isr0, isr1, ..., isr255).
 * Used by idt_init() to populate the IDT gate descriptors.
 */
extern uint64_t isr_stub_table[256];

/* =========================================================================
 * Handler registration
 * ========================================================================= */

/** Per-vector handler function pointer type. */
typedef void (*isr_handler_fn)(interrupt_frame_t *frame);

/**
 * @brief Initialize the ISR handler table (zero all entries).
 *
 * Must be called before isr_register_handler() and before the IDT
 * is installed.
 */
void isr_init(void);

/**
 * @brief Register a C handler for a specific interrupt vector.
 *
 * The handler is called with a pointer to the interrupt frame when the
 * corresponding vector fires.  The handler executes with interrupts
 * disabled (interrupt gate clears IF).
 *
 * @param vector  Interrupt vector number (0-255).
 * @param handler Function to call.  Pass NULL to unregister.
 */
void isr_register_handler(uint8_t vector, isr_handler_fn handler);

/**
 * @brief Unregister (clear) the C handler for a specific interrupt vector.
 *
 * Equivalent to isr_register_handler(vector, NULL).
 *
 * @param vector  Interrupt vector number (0-255).
 */
void isr_unregister_handler(uint8_t vector);

/**
 * @brief Common ISR dispatcher — called from every assembly stub.
 *
 * Dispatches to the registered per-vector handler (if any) and sends
 * EOI to the PIC for hardware IRQs (vectors 32-47).
 *
 * @param frame  Pointer to the interrupt frame on the stack.
 */
void isr_common_handler(interrupt_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_ISR_H */
