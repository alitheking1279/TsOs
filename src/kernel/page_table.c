/**
 * @file page_table.c
 * @brief Low-level x86-64 page table operations — walk, allocate, flush.
 *
 * This file provides the primitives that vmm.c builds on:
 *   - Physical-to-kernel-virtual address translation (identity map for now)
 *   - Page table entry read/write/clear
 *   - Walk from PML4 to a leaf PTE (with optional intermediate allocation)
 *   - TLB flush (single-page INVLPG and full CR3 reload)
 *   - Recursive table freeing
 *
 * Design constraints:
 *   - Every function that touches page table memory uses
 *     pt_phys_to_virt() for address conversion.  When the kernel
 *     migrates to a higher-half layout, only this one function changes.
 *   - All allocation goes through pmm_alloc_frame().  If the PMM is
 *     out of memory, the caller gets a NULL return, not a panic.
 *   - Serial logging is gated through an extern serial_dev_t pointer
 *     (set by vmm_init) so page_table.c degrades gracefully without
 *     serial.
 *
 * References:
 *   Intel SDM Vol.3A §4.5   — Paging structures
 *   Intel SDM Vol.3A §4.10  — Linear Address Translation
 */

#include "page_table.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Serial logging
 *
 * Same pattern as pmm.c: a static pointer set by vmm_init via
 * page_table_set_serial().  All logging is gated on this pointer.
 * ========================================================================= */

static serial_dev_t *g_pt_serial;

void page_table_set_serial(void *serial_dev) {
    g_pt_serial = (serial_dev_t *)serial_dev;
}

static void pt_log_str(const char *s) {
    if (g_pt_serial) serial_write_string(g_pt_serial, s);
}

static void pt_log_char(char c) {
    if (g_pt_serial) serial_write_char(g_pt_serial, c);
}

/** Print a 64-bit value as 0xHHHHHHHHHHHHHHHH (16 hex digits, zero-padded). */
static void pt_log_hex64(uint64_t val) {
    static const char hex[] = "0123456789ABCDEF";
    if (!g_pt_serial) return;
    serial_write_string(g_pt_serial, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_write_char(g_pt_serial, hex[(val >> i) & 0xF]);
    }
}

/** Print a decimal unsigned integer. */
static void pt_log_uint64(uint64_t val) {
    if (!g_pt_serial) return;
    if (val == 0) { serial_write_char(g_pt_serial, '0'); return; }
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    while (val > 0) { buf[--i] = (char)('0' + (val % 10)); val /= 10; }
    serial_write_string(g_pt_serial, &buf[i]);
}

/* =========================================================================
 * Address Conversion
 *
 * Currently an identity map: physical == kernel-virtual.
 * When the kernel moves to a higher-half layout, change the body of
 * these two functions and every page table operation automatically follows.
 * ========================================================================= */

void *pt_phys_to_virt(uint64_t phys) {
    /* Identity map: physical address == kernel-virtual address.
     * After higher-half migration, this becomes:
     *   return (void *)(phys + HIGHER_HALF_OFFSET); */
    return (void *)(uintptr_t)phys;
}

uint64_t pt_virt_to_phys(void *virt) {
    /* Identity map: kernel-virtual address == physical address.
     * After higher-half migration, this becomes:
     *   return (uint64_t)virt - HIGHER_HALF_OFFSET; */
    return (uint64_t)(uintptr_t)virt;
}

/* =========================================================================
 * TLB Flush Operations
 * ========================================================================= */

/**
 * @brief Flush a single TLB entry for the given virtual address.
 *
 * Executes the INVLPG instruction, which invalidates any TLB entry
 * that maps the specified linear address in the current PCID.
 * Also invalidates all paging-structure caches for the current PCID.
 *
 * Must be called after modifying, clearing, or freeing any PTE.
 */
void pt_invlpg(void *virt) {
    __asm__ volatile ("invlpg %0" : : "m"(*(volatile char *)virt) : "memory");
}

/**
 * @brief Flush the entire TLB by reloading CR3.
 *
 * Executes `mov cr3, rax` with the current CR3 value.  This flushes
 * all TLB entries for the current PCID except those marked Global
 * (PTE_GLOBAL).
 *
 * For most single-page operations, pt_invlpg() is preferred because
 * it is faster.  pt_flush_tlb() is used after bulk modifications or
 * when switching address spaces.
 */
void pt_flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* =========================================================================
 * Single Entry Operations
 * ========================================================================= */

/**
 * @brief Read a single page table entry.
 *
 * @param table  Pointer to the page table (in kernel-virtual memory).
 * @param index  Entry index (0-511).
 * @return The raw 64-bit PTE value, or 0 if table is NULL.
 */
uint64_t pt_get_entry(uint64_t *table, int index) {
    if (!table || index < 0 || index >= PT_ENTRIES) return 0;
    return table[index];
}

/**
 * @brief Write a single page table entry.
 *
 * @param table  Pointer to the page table (in kernel-virtual memory).
 * @param index  Entry index (0-511).
 * @param value  The PTE value to write.
 */
void pt_set_entry(uint64_t *table, int index, uint64_t value) {
    if (!table || index < 0 || index >= PT_ENTRIES) return;
    table[index] = value;
}

/**
 * @brief Clear a single page table entry to zero.
 *
 * @param table  Pointer to the page table (in kernel-virtual memory).
 * @param index  Entry index (0-511).
 */
void pt_clear_entry(uint64_t *table, int index) {
    if (!table || index < 0 || index >= PT_ENTRIES) return;
    table[index] = 0;
}

/**
 * @brief Check if all 512 entries in a page table are not-present.
 *
 * Used during unmapping to decide whether an intermediate table page
 * can be freed back to the PMM.
 *
 * @param table  Pointer to the page table (in kernel-virtual memory).
 * @return true if every entry is zero (not present), false otherwise.
 */
bool pt_table_empty(uint64_t *table) {
    if (!table) return true;
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (table[i] & PTE_PRESENT) return false;
    }
    return true;
}

/* =========================================================================
 * Page Table Allocation Helper
 * ========================================================================= */

/**
 * @brief Get an existing entry or create a new sub-table if not present.
 *
 * This is the core building block for page table walks with allocation.
 * If the entry at `table[index]` is present, it is returned directly.
 * If it is not present and `create` is true, a new page table page is
 * allocated from the PMM, zeroed, and installed with the given flags.
 *
 * @param table   Parent page table (in kernel-virtual memory).
 * @param index   Entry index (0-511) in the parent table.
 * @param create  If true, allocate a new sub-table when entry is empty.
 * @param flags   Flags to set on the new entry (PTE_PRESENT is mandatory).
 * @return The entry value (old or newly created), or 0 if allocation failed.
 */
static uint64_t pt_get_or_create_entry(uint64_t *table, int index,
                                       bool create, uint64_t flags) {
    uint64_t entry = pt_get_entry(table, index);

    /* Entry already present — return it as-is. */
    if (entry & PTE_PRESENT) {
        return entry;
    }

    /* Entry not present — create if requested. */
    if (!create) return 0;

    uint64_t new_phys = pmm_alloc_frame();
    if (new_phys == 0) {
        /* PMM out of memory. */
        pt_log_str("[PT]   pt_get_or_create_entry: OOM allocating table page\r\n");
        return 0;
    }

    /* Zero the newly allocated page table page. */
    void *new_virt = pt_phys_to_virt(new_phys);
    memset(new_virt, 0, PAGE_SIZE);

    /* Install the entry: physical address | flags.
     * The caller must include PTE_PRESENT in flags. */
    uint64_t new_entry = pte_make(new_phys, flags);
    pt_set_entry(table, index, new_entry);

    pt_log_str("[PT]   new table page @ ");
    pt_log_hex64(new_phys);
    pt_log_str(" installed at index ");
    pt_log_uint64((uint64_t)index);
    pt_log_str("\r\n");

    return new_entry;
}

/* =========================================================================
 * Page Table Walk
 * ========================================================================= */

/**
 * @brief Walk from PML4 down to a leaf PTE for the given virtual address.
 *
 * Performs a full 4-level page table walk.  At each intermediate level
 * (PML4 -> PDPT -> PD), if the entry is not present and `create` is
 * true, a new page table page is allocated and installed.
 *
 * Returns a pointer to the final PTE (at PT level) so the caller can
 * read or modify it.  For huge pages (PDPT with PS=1 for 1 GiB, or
 * PD with PS=1 for 2 MiB), the function returns a pointer to the
 * huge-page entry at that level instead.
 *
 * @param pml4    Pointer to the PML4 table (kernel-virtual).
 * @param vaddr   Target virtual address.
 * @param create  If true, allocate intermediate tables as needed.
 * @param flags   Flags for newly created intermediate entries
 *                (PTE_PRESENT | PTE_WRITABLE is standard).
 * @return Pointer to the PTE (or huge-page entry), or NULL if the
 *         walk fails (not present and !create, or PMM OOM).
 */
uint64_t *pt_walk_to_leaf(uint64_t *pml4, uint64_t vaddr, bool create,
                          uint64_t flags) {
    if (!pml4) return NULL;

    /* ---- Level 4: PML4 -> PDPT ---- */
    uint64_t pml4e = pt_get_or_create_entry(pml4, PML4_INDEX(vaddr),
                                             create, flags);
    if (!(pml4e & PTE_PRESENT)) return NULL;

    /* Sanity check: PML4 must never have PS=1 (reserved). */
    if (pml4e & PTE_PS) {
        pt_log_str("[PT] ERROR: PML4 entry has PS=1 (reserved bit set)\r\n");
        return NULL;
    }

    uint64_t *pdpt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pml4e));

    /* ---- Level 3: PDPT -> PD ---- */
    uint64_t pdpe = pt_get_or_create_entry(pdpt, PDPT_INDEX(vaddr),
                                            create, flags);
    if (!(pdpe & PTE_PRESENT)) return NULL;

    /* 1 GiB huge page — return pointer to this entry (caller handles). */
    if (pdpe & PTE_PS) {
        return &pdpt[PDPT_INDEX(vaddr)];
    }

    uint64_t *pd = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pdpe));

    /* ---- Level 2: PD -> PT ---- */
    uint64_t pde = pt_get_or_create_entry(pd, PD_INDEX(vaddr),
                                           create, flags);
    if (!(pde & PTE_PRESENT)) return NULL;

    /* 2 MiB huge page — return pointer to this entry (caller handles). */
    if (pde & PTE_PS) {
        return &pd[PD_INDEX(vaddr)];
    }

    uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pde));

    /* ---- Level 1: PT -> leaf PTE ---- */
    return &pt[PT_INDEX(vaddr)];
}

/* =========================================================================
 * Recursive Table Freeing
 * ========================================================================= */

/**
 * @brief Free a page table and all its sub-tables recursively.
 *
 * Walks an entire sub-tree of the page table hierarchy starting at
 * the given table, freeing every non-leaf page table page back to the
 * PMM.  Leaf PTEs are NOT freed (the caller is responsible for freeing
 * the physical frames they reference).
 *
 * @param table   Pointer to the table to free (kernel-virtual).
 * @param level   Current depth: 3=PML4(don't free), 2=PDPT, 1=PD, 0=PT.
 *                The PML4 itself is never freed by this function.
 */
void pt_free_recursive(uint64_t *table, int level) {
    if (!table || level <= 0) return;

    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t entry = table[i];
        if (!(entry & PTE_PRESENT)) continue;

        /* Never free huge pages — they don't point to sub-tables. */
        if (entry & PTE_PS) continue;

        uint64_t sub_phys = pte_addr(entry);
        void *sub_virt = pt_phys_to_virt(sub_phys);

        /* Recurse into sub-tables (level-1 = one level deeper). */
        pt_free_recursive((uint64_t *)sub_virt, level - 1);

        /* Free this sub-table page itself (not the entries above PML4). */
        pmm_free_frame(sub_phys);

        /* Clear the parent entry to avoid dangling references. */
        table[i] = 0;
    }
}

/* =========================================================================
 * Post-Unmap Cleanup
 * ========================================================================= */

/**
 * @brief Free intermediate page tables that became empty after an unmap.
 *
 * After vmm_unmap_page clears a leaf PTE, the PT (or PD, or PDPT) that
 * held it may now be entirely empty.  This function walks down from PML4
 * to locate the intermediate tables for `vaddr`, then walks back up
 * freeing any that are empty and clearing the parent entry.
 *
 * Safety: PML4 entries 0 (kernel identity) and 256-511 (kernel half)
 * are never cleared — only user-space entries (1-255) may be freed.
 *
 * @param pml4   PML4 table pointer (kernel-virtual).
 * @param vaddr  Virtual address whose intermediate tables to check.
 */
void pt_cleanup_empty_tables(uint64_t *pml4, uint64_t vaddr) {
    if (!pml4) return;

    /* Level 4: PML4 -> PDPT */
    int pml4_idx = PML4_INDEX(vaddr);
    uint64_t pml4e = pml4[pml4_idx];
    if (!(pml4e & PTE_PRESENT)) return;
    if (pml4e & PTE_PS) return;  /* shouldn't happen, but guard */
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pml4e));

    /* Level 3: PDPT -> PD */
    int pdpt_idx = PDPT_INDEX(vaddr);
    uint64_t pdpe = pdpt[pdpt_idx];
    if (!(pdpe & PTE_PRESENT)) return;
    if (pdpe & PTE_PS) return;   /* 1 GiB huge page, no sub-table */
    uint64_t *pd = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pdpe));

    /* Level 2: PD -> PT */
    int pd_idx = PD_INDEX(vaddr);
    uint64_t pde = pd[pd_idx];
    if (!(pde & PTE_PRESENT)) return;
    if (pde & PTE_PS) return;    /* 2 MiB huge page, no sub-table */
    uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pde));

    /* Level 1: check if PT is empty → free it, clear PD entry. */
    if (!pt_table_empty(pt)) return;

    pmm_free_frame(pte_addr(pde));
    pd[pd_idx] = 0;

    /* Level 2: check if PD is now empty → free it, clear PDPT entry. */
    if (!pt_table_empty(pd)) return;

    pmm_free_frame(pte_addr(pdpe));
    pdpt[pdpt_idx] = 0;

    /* Level 3: check if PDPT is now empty → free it, clear PML4 entry.
     * Only allowed for user-space PML4 entries (1-255). */
    if (!pt_table_empty(pdpt)) return;
    if (pml4_idx == 0 || pml4_idx >= 256) return;

    pmm_free_frame(pte_addr(pml4e));
    pml4[pml4_idx] = 0;
}

/* =========================================================================
 * Read CR3 (current page table root)
 * ========================================================================= */

/**
 * @brief Read the current CR3 register (physical address of PML4).
 *
 * Used by vmm_init() to capture the boot page table root before
 * any modifications.
 */
uint64_t pt_read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/**
 * @brief Load a new CR3 value (switch address space).
 *
 * Executes `mov cr3, rax`, which loads the new PML4 physical address
 * and flushes all non-global TLB entries.
 */
void pt_write_cr3(uint64_t cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
