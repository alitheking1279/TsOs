/**
 * @file page_table.h
 * @brief x86-64 4-level page table constants, entry flags, and manipulation
 *        helpers.
 *
 * This file defines the hardware page table format for IA-32e (long mode)
 * paging: PML4 -> PDPT -> PD -> PT.  Every constant and bit mask comes
 * from Intel SDM Vol.3A §4.5 (Paging) and §5.4 (Control Registers).
 *
 * Design principles:
 *   - Pure data types and inline helpers — no allocator dependencies.
 *   - Every bit position is named and referenced to the SDM section.
 *   - PTE manipulation helpers are static inline so the compiler can
 *     constant-fold them and there is zero function-call overhead.
 *   - Custom AVL bits (9-11) are reserved for OS-level flags (COW,
 *     demand paging) but defined here so both page_table.c and vmm.c
 *     share a single source of truth.
 *
 * 4-level virtual address decomposition (48-bit canonical):
 *
 *   63        48 47    39 38    30 29    21 20    12 11       0
 *   +-----------+--------+--------+--------+--------+---------+
 *   | sign ext  | PML4   | PDPT   |  PD    |  PT    | offset  |
 *   | (16 bit)  |(9 bit) |(9 bit) |(9 bit) |(9 bit) |(12 bit) |
 *   +-----------+--------+--------+--------+--------+---------+
 *
 * References:
 *   Intel SDM Vol.3A §4.5   — Paging structures and PTE formats
 *   Intel SDM Vol.3A §4.5.3 — PML4E, PDPE, PDE, PTE bit definitions
 *   Intel SDM Vol.3A §5.4   — Control registers (CR3, CR0.WP)
 *   Intel SDM Vol.3A §5.8   — Page Translation and Protection (flags)
 */

#ifndef KERNEL_PAGE_TABLE_H
#define KERNEL_PAGE_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pmm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Page Table Entry Flags — Intel SDM Vol.3A §4.5.3
 *
 * These constants apply at EVERY level of the page table hierarchy.
 * The CPU enforces the MOST RESTRICTIVE combination of flags across
 * all levels: if PML4E has R/W=0 and PTE has R/W=1, the page is
 * read-only.
 * ========================================================================= */

/** Bit 0: Page is present in physical memory. */
#define PTE_PRESENT     (1ULL << 0)

/** Bit 1: Page is writable.  When clear, writes trigger #PF. */
#define PTE_WRITABLE    (1ULL << 1)

/** Bit 2: Page is accessible from user mode (ring 3).  When clear,
 *  only supervisor mode (ring 0) may access the page. */
#define PTE_USER        (1ULL << 2)

/** Bit 3: Page Write-Through.  0 = write-back caching,
 *  1 = write-through caching. */
#define PTE_PWT         (1ULL << 3)

/** Bit 4: Page Cache Disable.  0 = caching enabled,
 *  1 = caching disabled (used for MMIO mappings). */
#define PTE_PCD         (1ULL << 4)

/** Bit 5: Accessed.  Set by hardware on ANY access (read or write).
 *  Hardware never clears this bit — the OS must clear it manually
 *  for LRU / page replacement algorithms. */
#define PTE_ACCESSED    (1ULL << 5)

/** Bit 6: Dirty.  Set by hardware on WRITE.  Only meaningful at the
 *  leaf (PT) level, or at PD level when PS=1 (2 MiB pages).
 *  Hardware never clears this bit. */
#define PTE_DIRTY       (1ULL << 6)

/** Bit 7: Page Size.
 *  - PML4 level: must be 0 (reserved).
 *  - PDPT level: 1 = 1 GiB huge page.
 *  - PD level: 1 = 2 MiB huge page.
 *  - PT level: this bit is PAT (Page Attribute Table index bit 0).
 *
 *  When PS=1 at a non-leaf level, the entry maps directly to physical
 *  memory instead of pointing to the next-level table. */
#define PTE_PS          (1ULL << 7)

/** Bit 8: Global.  When set, the TLB entry is NOT flushed on CR3
 *  reload.  Requires CR4.PGE=1.  Only meaningful at leaf level
 *  (and PD with PS=1, PDPT with PS=1).  Used for kernel mappings
 *  that are present in every address space. */
#define PTE_GLOBAL      (1ULL << 8)

/** Bits 9-11: AVL (Available to software, hardware ignores).
 *  We reserve these for OS-level metadata. */

/** AVL bit 9: Copy-on-Write marker.  When set, the page is shared
 *  with another address space and must be copied on write. */
#define PTE_COW         (1ULL << 9)

/** AVL bit 10: Demand-paged marker.  The PTE is present but the
 *  page has not yet been allocated — the first access triggers
 *  a page fault that the handler resolves by allocating a frame. */
#define PTE_DEMAND      (1ULL << 10)

/** Bit 12-51: Physical Frame Number (PFN).  Bits 12-51 hold the
 *  physical address of the page (or next-level table).  Only bits
 *  12-51 are used by hardware; bits 52-62 are reserved (or used
 *  for protection keys when CR4.PKE=1). */
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/** Bit 63: No-Execute.  1 = instruction fetches not allowed.
 *  Requires EFER.NXE=1 (MSR 0xC0000080 bit 11). */
#define PTE_NX          (1ULL << 63)

/* =========================================================================
 * Convenience: combined flag masks for common mapping types
 * ========================================================================= */

/** No permissions — page present but read-only, supervisor-only. */
#define PTE_PROT_NONE   (PTE_PRESENT)

/** Read-only, supervisor. */
#define PTE_PROT_READ   (PTE_PRESENT)

/** Read-write, supervisor. */
#define PTE_PROT_RW     (PTE_PRESENT | PTE_WRITABLE)

/** Read-write, user-accessible. */
#define PTE_PROT_RWU    (PTE_PRESENT | PTE_WRITABLE | PTE_USER)

/* =========================================================================
 * Virtual Address Decomposition Macros
 *
 * Extract the 9-bit index for each paging level from a virtual address.
 * See Intel SDM Vol.3A §4.5 Figure 4-8.
 * ========================================================================= */

/** PML4 index: bits [47:39] of the virtual address. */
#define PML4_INDEX(va)  (((uint64_t)(va) >> 39) & 0x1FF)

/** Page Directory Pointer Table index: bits [38:30]. */
#define PDPT_INDEX(va)  (((uint64_t)(va) >> 30) & 0x1FF)

/** Page Directory index: bits [29:21]. */
#define PD_INDEX(va)    (((uint64_t)(va) >> 21) & 0x1FF)

/** Page Table index: bits [20:12]. */
#define PT_INDEX(va)    (((uint64_t)(va) >> 12) & 0x1FF)

/** Byte offset within a 4 KiB page: bits [11:0]. */
#define PAGE_OFFSET(va) ((uint64_t)(va) & 0xFFF)

/** Number of entries per page table (512 entries × 8 bytes = 4096 bytes). */
#define PT_ENTRIES      512

/* =========================================================================
 * Inline PTE Manipulation Helpers
 *
 * These are used by both page_table.c (low-level walk) and vmm.c
 * (high-level map/unmap).  Being static inline, they compile to zero
 * overhead.
 * ========================================================================= */

/** Return true if the Present bit is set. */
static inline bool pte_present(uint64_t pte) {
    return (pte & PTE_PRESENT) != 0;
}

/** Return true if the Write bit is set. */
static inline bool pte_writable(uint64_t pte) {
    return (pte & PTE_WRITABLE) != 0;
}

/** Return true if the User bit is set. */
static inline bool pte_user(uint64_t pte) {
    return (pte & PTE_USER) != 0;
}

/** Return true if the Dirty bit is set (leaf entries only). */
static inline bool pte_dirty(uint64_t pte) {
    return (pte & PTE_DIRTY) != 0;
}

/** Return true if the Accessed bit is set. */
static inline bool pte_accessed(uint64_t pte) {
    return (pte & PTE_ACCESSED) != 0;
}

/** Return true if the Page Size bit is set (huge page indicator). */
static inline bool pte_huge(uint64_t pte) {
    return (pte & PTE_PS) != 0;
}

/** Return true if the Global bit is set. */
static inline bool pte_global(uint64_t pte) {
    return (pte & PTE_GLOBAL) != 0;
}

/** Return true if the No-Execute bit is set. */
static inline bool pte_nx(uint64_t pte) {
    return (pte & PTE_NX) != 0;
}

/** Return true if the COW (AVL bit 9) marker is set. */
static inline bool pte_cow(uint64_t pte) {
    return (pte & PTE_COW) != 0;
}

/** Return true if the Demand (AVL bit 10) marker is set. */
static inline bool pte_demand(uint64_t pte) {
    return (pte & PTE_DEMAND) != 0;
}

/** Extract the physical address (PFN + offset) from a PTE.
 *  Masks to bits 12-51, so the lower 12 bits of the result are zero
 *  (the caller must add the virtual address offset if needed). */
static inline uint64_t pte_addr(uint64_t pte) {
    return pte & PTE_ADDR_MASK;
}

/** Extract only the flag bits from a PTE (clears the address field). */
static inline uint64_t pte_flags(uint64_t pte) {
    return pte & ~PTE_ADDR_MASK;
}

/** Set a specific flag bit in a PTE. */
static inline uint64_t pte_set_flag(uint64_t pte, uint64_t flag) {
    return pte | flag;
}

/** Clear a specific flag bit in a PTE. */
static inline uint64_t pte_clear_flag(uint64_t pte, uint64_t flag) {
    return pte & ~flag;
}

/** Build a PTE value from a physical address and flags. */
static inline uint64_t pte_make(uint64_t phys_addr, uint64_t flags) {
    return (phys_addr & PTE_ADDR_MASK) | flags;
}

/** Check if a virtual address is in the upper (kernel) half.
 *  Upper half: bits [63:48] are all 1s (canonical). */
static inline bool va_is_kernel(uint64_t va) {
    return (va & 0xFFFF000000000000ULL) == 0xFFFF000000000000ULL;
}

/** Check if a virtual address is in the lower (user) half.
 *  Lower half: bits [63:48] are all 0s (canonical). */
static inline bool va_is_user(uint64_t va) {
    return (va & 0xFFFF000000000000ULL) == 0;
}

/** Check if a virtual address is canonical.
 *  Bits [63:48] must all equal bit 47. */
static inline bool va_is_canonical(uint64_t va) {
    if (va < 0x0000800000000000ULL) return true;  /* lower half */
    if (va >= 0xFFFF800000000000ULL) return true;  /* upper half */
    return false;                                   /* non-canonical */
}

/** Check if an address is page-aligned (lower 12 bits are zero). */
static inline bool is_page_aligned(uint64_t addr) {
    return (addr & (PAGE_SIZE - 1)) == 0;
}

/* =========================================================================
 * Kernel Heap Virtual Address Range
 *
 * The heap occupies a fixed VA region in kernel space, demand-paged:
 *   HEAP_BASE = 0xFFFFFFFFC0000000  (PML4[511] → PDPT[510] → PD[256])
 *   HEAP_END  = HEAP_BASE + 16 MiB  (4096 pages)
 *
 * This sits in PDPT[510] (the same PDPT that holds the kernel code at
 * PD[0-127]), but at PD[256] which is far above the kernel image.
 * The PD[0] 2 MiB huge page from boot is replaced with a normal PD
 * table on the first heap page fault — safe because the identity map
 * (PML4[0]) has already been removed and the kernel code lives at
 * PD[128+] (0xFFFFFFFF80200000+).
 *
 * All heap pages are demand-paged (PTE_DEMAND): no physical frames are
 * allocated until the first access, which triggers a page fault that
 * allocates a frame and installs the mapping.
 * ========================================================================= */

#define HEAP_BASE       0xFFFFFFFFC0000000ULL
#define HEAP_SIZE       (16ULL * 1024 * 1024)   /* 16 MiB */
#define HEAP_END        (HEAP_BASE + HEAP_SIZE)
#define HEAP_PAGE_COUNT (HEAP_SIZE / PAGE_SIZE)  /* 4096 pages */

/* =========================================================================
 * Physical <-> Kernel-Virtual Address Conversion
 *
 * The kernel is linked at a higher-half virtual address.  All physical
 * addresses passed to C code must be converted to kernel-virtual before
 * dereferencing.  pt_phys_to_virt / pt_virt_to_phys handle this.
 * ========================================================================= */

/** Offset between physical addresses and kernel-virtual addresses.
 *  Kernel VMA = physical + HIGHER_HALF_OFFSET.
 *  Matches the KERNEL_OFFSET in linker.ld. */
#define HIGHER_HALF_OFFSET 0xFFFFFFFF80000000ULL

/**
 * @brief Convert a physical address to a kernel-virtual pointer.
 *
 * Adds HIGHER_HALF_OFFSET so the result can be dereferenced in the
 * kernel's higher-half address space.
 */
void *pt_phys_to_virt(uint64_t phys);

/**
 * @brief Convert a kernel-virtual pointer to a physical address.
 *
 * Subtracts HIGHER_HALF_OFFSET to recover the original physical address.
 */
uint64_t pt_virt_to_phys(void *virt);

/**
 * @brief Set the serial device used by page_table.c logging.
 *
 * Must be called before any page table operations that produce log
 * output (typically from vmm_init).
 *
 * @param dev  Initialized serial device, or NULL to disable logging.
 */
void page_table_set_serial(void *serial_dev);

/* =========================================================================
 * TLB Flush Operations
 * ========================================================================= */

/**
 * @brief Flush a single TLB entry for the given virtual address.
 * Intel SDM Vol.3A §4.10.1 — INVLPG instruction.
 */
void pt_invlpg(void *virt);

/**
 * @brief Flush the entire TLB by reloading CR3.
 */
void pt_flush_tlb(void);

/* =========================================================================
 * Single Entry Operations
 * ========================================================================= */

/** Read a single page table entry. Returns 0 if table is NULL or index is out of range. */
uint64_t pt_get_entry(uint64_t *table, int index);

/** Write a single page table entry. */
void pt_set_entry(uint64_t *table, int index, uint64_t value);

/** Clear a single page table entry to zero. */
void pt_clear_entry(uint64_t *table, int index);

/** Check if all 512 entries in a page table are not-present. */
bool pt_table_empty(uint64_t *table);

/* =========================================================================
 * Page Table Walk
 * ========================================================================= */

/**
 * @brief Walk from PML4 down to a leaf PTE for the given virtual address.
 *
 * @param pml4    Pointer to the PML4 table (kernel-virtual).
 * @param vaddr   Target virtual address.
 * @param create  If true, allocate intermediate tables as needed.
 * @param flags   Flags for newly created intermediate entries.
 * @return Pointer to the leaf PTE, or NULL on failure.
 */
uint64_t *pt_walk_to_leaf(uint64_t *pml4, uint64_t vaddr, bool create,
                          uint64_t flags);

/**
 * @brief Free a page table and all its sub-tables recursively.
 *
 * Does NOT free the table itself — only its sub-tables. Leaf PTEs
 * (physical frames) are NOT freed; the caller manages those.
 *
 * @param table  Pointer to the table to free (kernel-virtual).
 * @param level  Current depth: 2=PDPT, 1=PD, 0=PT.
 */
void pt_free_recursive(uint64_t *table, int level);

/**
 * @brief Free intermediate page tables that became empty after an unmap.
 *
 * Walks from PML4 down to the leaf for vaddr, then checks each
 * intermediate table (PT, PD, PDPT) for emptiness.  Empty tables are
 * freed back to the PMM and the parent entry is cleared recursively.
 *
 * PML4 entries 0 and 256-511 are never cleared (kernel mappings).
 *
 * @param pml4   PML4 table pointer (kernel-virtual).
 * @param vaddr  Virtual address whose intermediate tables to check.
 */
void pt_cleanup_empty_tables(uint64_t *pml4, uint64_t vaddr);

/* =========================================================================
 * CR3 Operations
 * ========================================================================= */

/** Read the current CR3 register (physical address of PML4). */
uint64_t pt_read_cr3(void);

/** Load a new CR3 value (switch address space). */
void pt_write_cr3(uint64_t cr3);

/* =========================================================================
 * Address Space Structure
 *
 * Wraps the PML4 table (the root of the 4-level page table hierarchy)
 * along with its physical address (for loading into CR3) and a flag
 * indicating whether this is the kernel address space.
 *
 * The kernel address space is special:
 *   - It is created once during vmm_init() from the existing boot PML4.
 *   - Its upper 256 PML4 entries (kernel half) are shared with all
 *     user address spaces.
 *   - It is never destroyed.
 * ========================================================================= */

/**
 * @brief Virtual address space descriptor.
 *
 * Each address space has its own PML4 page (allocated from the PMM).
 * The PML4 is stored in kernel-virtual memory so the VMM can read and
 * modify it.  `pml4_phys` holds the physical address for CR3 loads.
 */
typedef struct {
    uint64_t *pml4;        /**< Kernel-virtual pointer to PML4 table. */
    uint64_t  pml4_phys;   /**< Physical address of PML4 (for CR3). */
    bool      is_kernel;   /**< true = kernel address space (never freed). */
} address_space_t;

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_PAGE_TABLE_H */
