/**
 * @file gdt.c
 * @brief Global Descriptor Table and Task State Segment initialization.
 *
 * Design principles:
 *   - Descriptor encoding is done with isolated helper functions, one per
 *     descriptor type, with every bit-field named and commented. No magic
 *     number byte arrays that are opaque to a future reader.
 *   - All static storage lives in .data (not .bss) so it is always valid
 *     before a BSS clear loop could run, and it is naturally zero-filled
 *     by the ELF loader for fields that must start at zero.
 *   - 16-byte alignment is applied to g_gdt and g_tss to avoid cache-line
 *     splits on structures the CPU accesses on every privilege transition.
 *   - No dynamic allocation: every structure is statically sized and laid
 *     out in the binary.
 *
 * Descriptor format references:
 *   Intel SDM Vol.3A §3.4.5  — Segment Descriptor bit layout
 *   Intel SDM Vol.3A §7.2.3  — 64-bit TSS system descriptor
 */

#include "gdt.h"
#include <stdint.h>

/* =========================================================================
 * Descriptor encoding helpers
 *
 * All descriptor fields use Intel SDM naming.  Each helper is intentionally
 * narrow — it only builds one logical descriptor type — so errors cannot
 * silently cross types (e.g. a data flag applied to a TSS entry).
 * ========================================================================= */

/**
 * @brief Encode one 8-byte code or data segment descriptor.
 *
 * In 64-bit long mode the base and limit are architecturally ignored for
 * code and data segments (flat model is mandatory), so base is always 0
 * and we use the maximum limit (0xFFFFF pages = full 4 GiB with G=1).
 *
 * Bit layout of the returned 64-bit value:
 *
 *   [15: 0] limit[15:0]   = 0xFFFF
 *   [31:16] base[15:0]    = 0x0000  (flat)
 *   [39:32] base[23:16]   = 0x00    (flat)
 *   [43:40] type          = 0xA (exec+read) or 0x2 (data read/write)
 *   [44]    S             = 1        (code/data, not system)
 *   [46:45] DPL           = dpl
 *   [47]    P             = 1        (present)
 *   [51:48] limit[19:16]  = 0xF
 *   [52]    AVL           = 0
 *   [53]    L             = 1 for code in 64-bit mode, 0 for data
 *   [54]    D/B           = 0 when L=1 (required); 1 for data (32-bit default)
 *   [55]    G             = 1        (page granularity, limit × 4096)
 *   [63:56] base[31:24]   = 0x00    (flat)
 *
 * @param dpl   Descriptor Privilege Level: 0 = ring 0, 3 = ring 3.
 * @param exec  Non-zero → code segment (execute/read, L=1).
 *              Zero     → data segment (read/write, D/B=1).
 */
static uint64_t encode_segment_descriptor(int dpl, int exec) {
    uint64_t desc = 0;

    /* Limit: maximum (0xFFFFF) expressed as two non-contiguous fields. */
    desc |= (uint64_t)0x0000FFFF;              /* bits [15: 0] — limit[15:0]  */
    desc |= (uint64_t)0xF << 48;              /* bits [51:48] — limit[19:16] */

    /* Base: 0 (flat addressing — all base fields remain zero). */

    /* Type field (bits [43:40]):
     *   0xA = execute/read (non-conforming, accessed bit clear)
     *   0x2 = read/write   (expand-up data, accessed bit clear) */
    desc |= (exec ? (uint64_t)0xA : (uint64_t)0x2) << 40;

    /* S=1: this is a code/data segment, not a system descriptor. */
    desc |= (uint64_t)1 << 44;

    /* DPL (bits [46:45]). */
    desc |= ((uint64_t)(dpl & 0x3)) << 45;

    /* P=1: segment is present in memory. */
    desc |= (uint64_t)1 << 47;

    if (exec) {
        /* L=1: 64-bit code segment; D/B must be 0 when L=1 (SDM §3.4.5). */
        desc |= (uint64_t)1 << 53;   /* L bit */
        /* bit 54 (D/B) left as 0 — required when L=1 */
    } else {
        /* Data segment: D/B=1 selects 32-bit default operand/stack size.
         * In 64-bit mode data segments are not used for addressing, but
         * setting D/B=1 produces a canonical "flat 32-bit" descriptor
         * that is valid and avoids legacy-mode surprises. */
        desc |= (uint64_t)1 << 54;   /* D/B bit */
    }

    /* G=1: the 20-bit limit field is in 4 KiB pages, so effective limit
     *       = (limit + 1) × 4096 − 1 = 0xFFFFF×4096+4095 = 4 GiB − 1. */
    desc |= (uint64_t)1 << 55;

    return desc;
}

/**
 * @brief Encode the low 8 bytes of a 64-bit TSS system descriptor.
 *
 * A 64-bit TSS descriptor is 16 bytes (two consecutive GDT slots).
 * This function fills the lower slot (GDT[5]).
 *
 * Bit layout:
 *   [15: 0] limit[15:0]
 *   [31:16] base[15:0]
 *   [39:32] base[23:16]
 *   [43:40] type = 0x9  (Available 64-bit TSS)
 *   [44]    S    = 0    (system descriptor)
 *   [46:45] DPL  = 0    (kernel only)
 *   [47]    P    = 1    (present)
 *   [51:48] limit[19:16]
 *   [52]    AVL  = 0
 *   [53]    0    (reserved for TSS)
 *   [54]    0    (reserved for TSS)
 *   [55]    G    = 0    (byte granularity — TSS limit is in bytes)
 *   [63:56] base[31:24]
 *
 * @param base   Linear address of the tss64_t structure.
 * @param limit  Byte limit of the TSS (sizeof(tss64_t) - 1 = 103).
 */
static uint64_t encode_tss_descriptor_low(uint64_t base, uint32_t limit) {
    uint64_t desc = 0;

    /* Limit fields (byte granularity, G=0 → value is not scaled). */
    desc |= (uint64_t)(limit & 0x0000FFFF);          /* bits [15: 0] — limit[15:0]  */
    desc |= (uint64_t)((limit >> 16) & 0xF) << 48;  /* bits [51:48] — limit[19:16] */

    /* Base fields (split across four non-contiguous byte positions). */
    desc |= (uint64_t)( base        & 0x0000FFFF) << 16; /* bits [31:16] — base[15:0]  */
    desc |= (uint64_t)((base >> 16) & 0x000000FF) << 32; /* bits [39:32] — base[23:16] */
    desc |= (uint64_t)((base >> 24) & 0x000000FF) << 56; /* bits [63:56] — base[31:24] */

    /* Type = 0x9: Available 64-bit TSS (SDM §7.2.2 Table 7-1). */
    desc |= (uint64_t)0x9 << 40;

    /* S=0 (system descriptor) — bit 44 left as zero. */
    /* DPL=0 — bits [46:45] left as zero. */

    /* P=1: descriptor is present. */
    desc |= (uint64_t)1 << 47;

    /* G=0: limit is in bytes, not pages — bit 55 left as zero. */

    return desc;
}

/**
 * @brief Encode the high 8 bytes of a 64-bit TSS system descriptor.
 *
 * Contains only the upper 32 bits of the base address; bits [63:32]
 * of this slot are reserved and must be zero (hardware checks this).
 *
 * @param base  Same linear address passed to encode_tss_descriptor_low().
 */
static uint64_t encode_tss_descriptor_high(uint64_t base) {
    /* bits [31:0] of high slot = base[63:32].
     * bits [63:32] of high slot = 0 (reserved — must not be set). */
    return (base >> 32) & 0x00000000FFFFFFFFULL;
}

/* =========================================================================
 * Static storage
 *
 * Both g_gdt and g_tss are placed in .data so the ELF loader initialises
 * them to zero before kernel_main is called (satisfying the "reserved fields
 * must be zero" requirement without an explicit memset).
 *
 * Aligned(16): avoids cache-line splits on structures the CPU touches on
 * every interrupt or privilege transition.
 * ========================================================================= */

static uint64_t g_gdt[GDT_ENTRY_COUNT] __attribute__((section(".data"), aligned(16)));
static tss64_t  g_tss                  __attribute__((section(".data"), aligned(16)));

/**
 * GDTR pseudo-descriptor — {uint16_t limit, uint64_t base}, packed.
 * Must be packed: lgdt reads exactly 10 bytes at the given address.
 */
typedef struct {
    uint16_t limit; /**< Byte length of GDT minus 1. */
    uint64_t base;  /**< Linear (virtual) address of GDT[0]. */
} __attribute__((packed)) gdt_pointer_t;

static gdt_pointer_t g_gdt_ptr;

/* Provided by gdt_flush.asm — must match the SysV AMD64 ABI. */
extern void gdt_flush(gdt_pointer_t *ptr, uint64_t cs, uint64_t ds);
extern void tss_flush(uint64_t tss_sel);

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

void gdt_init(void) {
    /* ------------------------------------------------------------------
     * Build the six logical segment descriptors.
     * ------------------------------------------------------------------ */

    g_gdt[0] = 0;                                   /* [0] null  — must be zero   */
    g_gdt[1] = encode_segment_descriptor(0, 1);     /* [1] kcode — ring 0, exec   */
    g_gdt[2] = encode_segment_descriptor(0, 0);     /* [2] kdata — ring 0, data   */
    g_gdt[3] = encode_segment_descriptor(3, 0);     /* [3] udata — ring 3, data
                                                     *     one index below ucode
                                                     *     so SYSRET SS = base+8
                                                     *     lands here */
    g_gdt[4] = encode_segment_descriptor(3, 1);     /* [4] ucode — ring 3, exec
                                                     *     SYSRET CS = base+16
                                                     *     lands here */

    /* ------------------------------------------------------------------
     * Build the 16-byte TSS system descriptor (two contiguous GDT slots).
     * ------------------------------------------------------------------ */

    uint64_t tss_base  = (uint64_t)&g_tss;
    uint32_t tss_limit = (uint32_t)(sizeof(tss64_t) - 1U); /* = 103 */

    g_gdt[5] = encode_tss_descriptor_low (tss_base, tss_limit);
    g_gdt[6] = encode_tss_descriptor_high(tss_base);

    /* ------------------------------------------------------------------
     * TSS: set I/O permission bitmap offset past the end of the TSS,
     * which means all I/O port access from ring 3 will #GP.
     * ------------------------------------------------------------------ */
    g_tss.iomap_base = (uint16_t)sizeof(tss64_t); /* = 104 */

    /* ------------------------------------------------------------------
     * Build the GDTR pseudo-descriptor, install via lgdt + far-return.
     * ------------------------------------------------------------------ */
    g_gdt_ptr.limit = (uint16_t)(sizeof(g_gdt) - 1U); /* GDT_ENTRY_COUNT*8-1 = 55 */
    g_gdt_ptr.base  = (uint64_t)g_gdt;

    /* gdt_flush() performs:  lgdt; retfq (reload CS); reload DS/ES/FS/GS/SS */
    gdt_flush(&g_gdt_ptr, GDT_KERNEL_CS_SEL, GDT_KERNEL_DS_SEL);

    /* Load the Task Register — must come after gdt_flush so the GDTR and
     * the TSS descriptor are in place before ltr reads them. */
    tss_flush(GDT_TSS_SEL);
}

void tss_set_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}

void tss_set_ist(int entry, uint64_t rsp) {
    if (entry >= 0 && entry < 7) {
        g_tss.ist[entry] = rsp;
    }
}

uint64_t gdt_get_address(void) {
    return (uint64_t)g_gdt;
}

uint64_t gdt_get_tss_address(void) {
    return (uint64_t)&g_tss;
}
