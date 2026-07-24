/**
 * @file gdt.h
 * @brief Global Descriptor Table (GDT) and Task State Segment (TSS) interface.
 *
 * GDT layout (7 raw uint64_t entries = 56 bytes, GDTR limit = 55):
 *
 *   Index  Selector  DPL  Width  Type        Description
 *   -----  --------  ---  -----  ----------  ----------------------------
 *     0      0x00     -    -     null        Required; never referenced
 *     1      0x08     0   64-bit code        Kernel execute/read, L=1
 *     2      0x10     0   flat   data        Kernel read/write
 *     3      0x18     3   64-bit code        User execute/read, L=1
 *     4      0x20     3   flat   data        User read/write
 *     5+6    0x28     0   16B    TSS system  Available 64-bit TSS
 *
 * The TSS descriptor occupies two consecutive 8-byte GDT slots (indices 5
 * and 6) because the full 64-bit base address cannot fit in a single 8-byte
 * segment descriptor — this two-slot layout is mandated by the CPU hardware
 * (Intel SDM Vol.3A §7.2.3).
 *
 * Segment selector encoding:  index[15:3] | TI[2]=0 (GDT) | RPL[1:0]
 * User selectors include RPL=3, e.g. index 3 → 0x18 | 3 = 0x1B.
 *
 * References:
 *   Intel SDM Vol.3A §3.4.5  — Segment Descriptor formats
 *   Intel SDM Vol.3A §7.7    — Task State Segment descriptor (64-bit)
 *   OSDev wiki — GDT Tutorial
 */

#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Segment selectors
 * ========================================================================= */

/** Null segment — must never appear in a live segment register. */
#define GDT_NULL_SEL         ((uint16_t)0x00)

/** Kernel code segment: GDT index 1, TI=0, RPL=0 → selector 0x08. */
#define GDT_KERNEL_CS_SEL    ((uint16_t)0x08)

/** Kernel data segment: GDT index 2, TI=0, RPL=0 → selector 0x10. */
#define GDT_KERNEL_DS_SEL    ((uint16_t)0x10)

/** User code segment: GDT index 3, TI=0, RPL=3 → selector 0x1B. */
#define GDT_USER_CS_SEL      ((uint16_t)0x1B)

/** User data segment: GDT index 4, TI=0, RPL=3 → selector 0x23. */
#define GDT_USER_DS_SEL      ((uint16_t)0x23)

/**
 * TSS segment: GDT index 5, TI=0, RPL=0 → selector 0x28.
 * Occupies GDT entries [5] and [6] (16 bytes total in the GDT).
 */
#define GDT_TSS_SEL          ((uint16_t)0x28)

/* =========================================================================
 * GDT size constant
 * ========================================================================= */

/**
 * Number of raw 8-byte entries in the GDT array.
 *   5 normal descriptors + 2 slots for the 16-byte TSS system descriptor = 7.
 *   GDTR.limit = GDT_ENTRY_COUNT * 8 - 1 = 55.
 */
#define GDT_ENTRY_COUNT      7

/* =========================================================================
 * Task State Segment (x86-64, 104 bytes)
 * ========================================================================= */

/**
 * @brief 64-bit TSS structure (Intel SDM Vol.3A §7.7, Figure 7-11).
 *
 * The CPU accesses this memory directly on privilege transitions and IST
 * stack switches. __attribute__((packed)) prevents the compiler from
 * inserting padding that would misalign CPU-expected field offsets.
 *
 * Fields required by the hardware are named explicitly; the rest are zero.
 */
typedef struct {
    uint32_t reserved0;     /**< [+0]   Reserved, must be zero. */
    uint64_t rsp0;          /**< [+4]   Ring-0 RSP — loaded on ring3→ring0 entry. */
    uint64_t rsp1;          /**< [+12]  Ring-1 RSP — unused in a flat kernel. */
    uint64_t rsp2;          /**< [+20]  Ring-2 RSP — unused in a flat kernel. */
    uint64_t reserved1;     /**< [+28]  Reserved, must be zero. */
    uint64_t ist[7];        /**< [+36]  IST1–IST7 (Interrupt Stack Table pointers). */
    uint64_t reserved2;     /**< [+92]  Reserved, must be zero. */
    uint16_t reserved3;     /**< [+100] Reserved, must be zero. */
    uint16_t iomap_base;    /**< [+102] Byte offset from TSS base to I/O permission
                                         bitmap. Set to sizeof(tss64_t) = 104 to
                                         indicate no bitmap (all I/O disallowed). */
} __attribute__((packed)) tss64_t;

/** Compile-time assertion: the Intel SDM specifies the 64-bit TSS is
 *  exactly 104 bytes.  If the struct padding or field sizes are wrong,
 *  this typedef will fail to compile with an "array size is negative" error. */
typedef char _tss64_size_check[(sizeof(tss64_t) == 104) ? 1 : -1];

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Build and install the kernel GDT and TSS.
 *
 * Steps performed:
 *   1. Encode all six logical segment descriptors into g_gdt[].
 *   2. Encode the two-slot 64-bit TSS system descriptor (g_gdt[5+6]).
 *   3. Set TSS.iomap_base to indicate no I/O permission bitmap.
 *   4. Write the GDTR pseudo-descriptor and call gdt_flush() (lgdt +
 *      far-return to reload CS + reload DS/ES/FS/GS/SS).
 *   5. Call tss_flush() (ltr) to load the Task Register.
 *
 * Post-conditions after return:
 *   - GDTR base == &g_gdt, GDTR limit == GDT_ENTRY_COUNT*8-1 (= 55).
 *   - CS  == GDT_KERNEL_CS_SEL (0x08).
 *   - DS/ES/FS/GS/SS == GDT_KERNEL_DS_SEL (0x10).
 *   - TR  == GDT_TSS_SEL (0x28); TSS type promoted to Busy (0xB) by hardware.
 *   - TSS.RSP0 == 0; call tss_set_rsp0() before enabling ring-3 code.
 *
 * Must be called before enabling interrupts or handling any faults, so
 * that exception/IRQ entry sees a valid GDT and a loaded TSS.
 */
void gdt_init(void);

/**
 * @brief Update the ring-0 stack pointer stored in the TSS.
 *
 * On any ring-3 → ring-0 privilege transition (interrupt, fault, syscall),
 * the CPU atomically loads RSP from TSS.RSP0.  Call this function whenever
 * the active kernel stack changes (e.g., on every task switch) so the CPU
 * always switches to a valid stack.
 *
 * @param rsp0  Linear address of the top of the new ring-0 stack.
 */
void tss_set_rsp0(uint64_t rsp0);

/**
 * @brief Update an Interrupt Stack Table (IST) entry in the TSS.
 *
 * IST entries provide alternate stack pointers for specific IDT gate
 * descriptors.  When a gate's IST index field is non-zero, the CPU
 * unconditionally switches to the corresponding IST stack on entry,
 * regardless of the current privilege level.  This is essential for
 * handlers that must run on a known-good stack (e.g. double-fault
 * handler when the kernel stack may be corrupted).
 *
 * @param entry  IST index (0 = IST1, 6 = IST7).  Valid range: 0-6.
 * @param rsp    Linear address of the TOP of the IST stack (stacks
 *               grow downward on x86).
 */
void tss_set_ist(int entry, uint64_t rsp);

/* =========================================================================
 * Introspection helpers — used by test_gdt.c
 * ========================================================================= */

/**
 * @return Linear address of the static g_gdt[] array.
 *         This is the value written into the GDTR base field by gdt_init().
 */
uint64_t gdt_get_address(void);

/**
 * @return Linear address of the static g_tss structure.
 *         This is the value encoded into GDT entries [5] and [6] as the
 *         TSS descriptor base.
 */
uint64_t gdt_get_tss_address(void);

/* =========================================================================
 * MSRs for SYSCALL/SYSRET (Intel SDM Vol.3A §3.3.1)
 * ========================================================================= */

/** Extended Feature Enable Register. */
#define MSR_EFER            0xC0000080

/** SYSCALL target address in long mode.
 *  Bits [63:32] = target RIP, bits [31:16] = kernel CS, bits [15:0] = user CS. */
#define MSR_STAR            0xC0000081

/** SYSCALL RIP — target RIP for SYSCALL in long mode. */
#define MSR_LSTAR           0xC0000082

/** SYSCALL RFLAGS Mask — bits to clear in RFLAGS on SYSCALL entry. */
#define MSR_SFMASK          0xC0000084

/** Kernel GS Base — used for per-CPU data access via SWAPGS. */
#define MSR_GS_BASE         0xC0000101

/** SYSCALL entry point address (defined in syscall_entry.asm). */
extern uint64_t syscall_entry_addr;

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_GDT_H */
