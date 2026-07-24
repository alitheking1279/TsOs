/**
 * @file vmm.h
 * @brief Virtual Memory Manager — public API.
 *
 * The VMM builds on the page table primitives (page_table.h/c) to provide
 * a high-level interface for managing virtual address spaces:
 *   - Map a virtual address to a physical frame
 *   - Unmap a virtual address
 *   - Translate a virtual address to physical
 *   - Create and destroy independent address spaces
 *   - Clone an address space (for fork)
 *   - Switch between address spaces
 *
 * Design principles:
 *   - Every function takes an explicit address_space_t* — no global state.
 *   - Every function returns a vmm_status_t error code — no panics.
 *   - Every function validates inputs before touching page tables.
 *   - Serial logging traces every operation for debugging.
 *
 * Higher-half layout: kernel linked at 0xFFFFFFFF80000000.  All
 * kernel-physical addresses are accessed through pt_phys_to_virt().
 * After all init, the identity map (PML4[0]) is removed.
 *
 * References:
 *   Intel SDM Vol.3A §4.5   — Paging structures
 *   Intel SDM Vol.3A §4.10  — Linear Address Translation
 *   Intel SDM Vol.3A §6.15  — Page-Fault Exception
 */

#ifndef KERNEL_VMM_H
#define KERNEL_VMM_H

#include "page_table.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * VMM Status Codes
 * ========================================================================= */

typedef enum {
    VMM_OK              =  0,   /**< Success. */
    VMM_ERR_NOT_INIT    = -1,   /**< vmm_init() has not been called. */
    VMM_ERR_INVALID     = -2,   /**< Bad address, alignment, or argument. */
    VMM_ERR_NOT_MAPPED  = -3,   /**< Virtual address has no mapping. */
    VMM_ERR_ALREADY_MAP = -4,   /**< Virtual address already mapped. */
    VMM_ERR_NO_MEM      = -5,   /**< PMM allocation failure (OOM). */
    VMM_ERR_PERM        = -6,   /**< Permission denied. */
    VMM_ERR_HUGE_PAGE   = -7,   /**< Cannot split a huge page mapping. */
} vmm_status_t;

/* =========================================================================
 * Mapping Flags
 *
 * These are convenience aliases for the raw PTE flags defined in
 * page_table.h.  The VMM uses these as the `flags` parameter to
 * vmm_map_page(); they are OR'd together and stored directly in the
 * PTE.
 * ========================================================================= */

#define VMM_FLAG_WRITE      PTE_WRITABLE    /**< Page is writable. */
#define VMM_FLAG_USER       PTE_USER        /**< User-mode accessible. */
#define VMM_FLAG_NOEXEC     PTE_NX          /**< No-execute (requires EFER.NXE). */
#define VMM_FLAG_WRITE_THRU PTE_PWT         /**< Write-through caching. */
#define VMM_FLAG_CACHE_DIS  PTE_PCD         /**< Cache disabled (MMIO). */
#define VMM_FLAG_GLOBAL     PTE_GLOBAL      /**< Global (not flushed on CR3 switch). */

/* =========================================================================
 * Test VA Range
 *
 * A safe virtual address range for VMM tests.  This sits in user space
 * (lower half), in the PML4[1] region (512 GiB range) which is not
 * shared with the kernel and not pre-mapped by boot page tables.
 *
 * Tests use addresses in [VMM_TEST_VA_BASE, VMM_TEST_VA_BASE + 8 MiB).
 * Each test allocates its own physical frames from the PMM and maps
 * them into this range.
 * ========================================================================= */

#define VMM_TEST_VA_BASE    0x0000008000000000ULL  /* 512 GiB — PML4[1], not shared with kernel */
#define VMM_TEST_VA_END     0x0000008040000000ULL  /* 512 GiB + 4 MiB — 4 MiB test window */
#define VMM_TEST_VA_STEP    0x1000ULL       /* 4 KiB — one page per step */

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the Virtual Memory Manager.
 *
 * Captures the current CR3 (boot page table), sets up the kernel
 * address space descriptor, and initializes the logging subsystem.
 * Must be called after serial_init() and pmm_init().
 *
 * @param serial_dev  Initialized serial device for logging.  May be NULL.
 */
void vmm_init(void *serial_dev);

/**
 * @brief Create a new independent user address space.
 *
 * Allocates a fresh PML4 page, zeros it, and copies the upper 256
 * entries (kernel half) from the kernel address space.  The lower
 * 256 entries (user half) start empty.
 *
 * @param[out] out  Populated with the new address_space_t on success.
 * @return VMM_OK on success, VMM_ERR_NO_MEM if PMM is exhausted.
 */
vmm_status_t vmm_create_address_space(address_space_t *out);

/**
 * @brief Destroy an address space and free all its resources.
 *
 * Frees every intermediate page table page AND every physical frame
 * mapped in the user half back to the PMM.  The kernel address space
 * cannot be destroyed.
 *
 * @param as  Address space to destroy.
 * @return VMM_OK on success, VMM_ERR_INVALID if NULL or kernel space.
 */
vmm_status_t vmm_destroy_address_space(address_space_t *as);

/**
 * @brief Map a virtual address to a physical address.
 *
 * Walks the page table hierarchy, allocating intermediate tables as
 * needed, and installs a leaf PTE mapping vaddr -> paddr with the
 * given flags.  TLB is flushed for the mapped page.
 *
 * @param as     Target address space.
 * @param vaddr  Virtual address (must be page-aligned, canonical).
 * @param paddr  Physical address (must be page-aligned).
 * @param flags  PTE flags (VMM_FLAG_WRITE, VMM_FLAG_USER, etc.).
 * @return VMM_OK on success, or an error code.
 */
vmm_status_t vmm_map_page(address_space_t *as, uint64_t vaddr,
                          uint64_t paddr, uint64_t flags);

/**
 * @brief Unmap a virtual address.
 *
 * Clears the leaf PTE and flushes the TLB.  If the page table page
 * containing the PTE becomes empty, it is freed back to the PMM
 * (and the parent entry is cleared recursively up to PML4).
 *
 * @param as     Target address space.
 * @param vaddr  Virtual address to unmap.
 * @return VMM_OK on success, VMM_ERR_NOT_MAPPED if not mapped.
 */
vmm_status_t vmm_unmap_page(address_space_t *as, uint64_t vaddr);

/**
 * @brief Unmap a range of virtual addresses.
 *
 * Calls vmm_unmap_page() for every page in [vaddr_start, vaddr_end).
 * Skips addresses that are not mapped.
 *
 * @param as           Target address space.
 * @param vaddr_start  Start of range (page-aligned).
 * @param vaddr_end    End of range (page-aligned, exclusive).
 * @return VMM_OK on success.
 */
vmm_status_t vmm_unmap_range(address_space_t *as, uint64_t vaddr_start,
                             uint64_t vaddr_end);

/**
 * @brief Translate a virtual address to its physical address.
 *
 * Walks all 4 levels without modifying anything.  Handles huge pages
 * at PDPT (1 GiB) and PD (2 MiB) levels.
 *
 * @param as     Address space to query.
 * @param vaddr  Virtual address to translate.
 * @return Physical address, or 0 if not mapped.
 */
uint64_t vmm_translate(address_space_t *as, uint64_t vaddr);

/**
 * @brief Clone an address space (deep copy of page table structure).
 *
 * Creates a new address space with the same mappings as `src`.
 * Kernel mappings are shared (same PML4 entries).  User page table
 * pages are newly allocated and copied (512 PTEs per table).
 * Physical page frames are shared (not duplicated) — only the
 * page table structure is cloned.
 *
 * @param src  Source address space.
 * @param dst  Destination (must be a freshly created address space).
 * @return VMM_OK on success.
 */
vmm_status_t vmm_clone_address_space(address_space_t *src,
                                     address_space_t *dst);

/**
 * @brief Switch to a different address space.
 *
 * Loads the address space's PML4 physical address into CR3.
 * Flushes all non-global TLB entries.
 *
 * @param as  Address space to switch to.
 */
void vmm_switch_address_space(address_space_t *as);

/**
 * @brief Get the kernel address space descriptor.
 *
 * @return Pointer to the static kernel address space, or NULL if
 *         vmm_init() has not been called.
 */
address_space_t *vmm_get_kernel_address_space(void);

/**
 * @brief Check whether the VMM has been initialized.
 *
 * @return true if vmm_init() has been called, false otherwise.
 */
bool vmm_is_initialized(void);

/**
 * @brief Unmap a virtual address and free the underlying physical frame.
 *
 * Combines vmm_unmap_page() with PMM frame release.  Walks the page
 * tables to find the leaf PTE, extracts the physical address, frees the
 * frame to the PMM, clears the PTE, and flushes TLB.
 *
 * @param as     Target address space.
 * @param vaddr  Virtual address to unmap and free.
 * @return VMM_OK on success, VMM_ERR_NOT_MAPPED if not mapped.
 */
vmm_status_t vmm_unmap_and_free(address_space_t *as, uint64_t vaddr);

/**
 * @brief Map a virtual address with a DEMAND PTE (lazy allocation).
 *
 * Creates a page table entry that is NOT present but has PTE_DEMAND
 * set.  When the page is first accessed, the page fault handler
 * allocates a physical frame and resolves the mapping automatically.
 *
 * Used for user stacks and heap — avoids allocating physical memory
 * for pages that may never be touched.
 *
 * @param as     Target address space.
 * @param vaddr  Virtual address (must be page-aligned, user half).
 * @param flags  PTE flags (without PTE_PRESENT — e.g. VMM_FLAG_WRITE | VMM_FLAG_USER).
 * @return VMM_OK on success, or an error code.
 */
vmm_status_t vmm_map_demand_page(address_space_t *as, uint64_t vaddr,
                                 uint64_t flags);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_VMM_H */
