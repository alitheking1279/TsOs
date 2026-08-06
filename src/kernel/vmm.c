/**
 * @file vmm.c
 * @brief Virtual Memory Manager — address space and page mapping core.
 *
 * This is the high-level VMM that builds on the page table primitives
 * in page_table.c.  It manages:
 *   - A static kernel address space (captured from the boot page table)
 *   - User address space creation (new PML4 + kernel mapping copy)
 *   - Page mapping and unmapping with full TLB management
 *   - Virtual-to-physical translation
 *   - Address space cloning (for fork)
 *
 * Every function validates inputs, traces through serial logging, and
 * returns a status code — no panics, no assertions, no global state
 * beyond the kernel address space.
 *
 * Init order in kernel_main:
 *   serial_init -> isr_init -> gdt_init -> pic_init -> idt_init
 *   -> register_fault_handlers -> pmm_init -> vmm_init -> tests
 */

#include "vmm.h"
#include "vmm_log.h"
#include "page_table.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Static state
 * ========================================================================= */

/** Kernel address space — captured from the boot page table at init. */
static address_space_t g_kernel_aspace;

/** True after vmm_init() completes successfully. */
static bool g_vmm_initialized = false;

/** Default flags for newly created intermediate page table entries.
 *  PRESENT | WRITABLE — intermediate tables are always writable. */
#define PT_DEFAULT_FLAGS (PTE_PRESENT | PTE_WRITABLE)

/* =========================================================================
 * VMM Log Implementation
 *
 * These functions implement the logging API declared in vmm_log.h.
 * They follow the same serial-gating pattern as pmm.c.
 * ========================================================================= */

static serial_dev_t *g_vmm_serial;

/** Level name strings for the log prefix. */
static const char * const g_level_names[] = {
    "DBG", "INF", "WRN", "ERR"
};

void vmm_log_init(void *serial_dev) {
    g_vmm_serial = (serial_dev_t *)serial_dev;
}

void vmm_log_write(int level, const char *msg) {
    if (!g_vmm_serial) return;
    if (level < 0 || level > 3) level = 3;

    /* Prefix: [VMM:XXX] */
    serial_write_char(g_vmm_serial, '[');
    serial_write_string(g_vmm_serial, "VMM:");
    serial_write_string(g_vmm_serial, g_level_names[level]);
    serial_write_char(g_vmm_serial, ']');
    serial_write_char(g_vmm_serial, ' ');

    /* Message body. */
    if (msg) serial_write_string(g_vmm_serial, msg);
}

void vmm_log_hex64(uint64_t val) {
    if (!g_vmm_serial) return;
    static const char hex[] = "0123456789ABCDEF";
    serial_write_string(g_vmm_serial, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_write_char(g_vmm_serial, hex[(val >> i) & 0xF]);
    }
}

void vmm_log_uint64(uint64_t val) {
    if (!g_vmm_serial) return;
    if (val == 0) { serial_write_char(g_vmm_serial, '0'); return; }
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    while (val > 0) { buf[--i] = (char)('0' + (val % 10)); val /= 10; }
    serial_write_string(g_vmm_serial, &buf[i]);
}

void vmm_log_char(char c) {
    if (g_vmm_serial) serial_write_char(g_vmm_serial, c);
}

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

/** Build a human-readable flag string for a PTE (e.g. "RW-UG-"). */
static void format_flags(uint64_t flags, char *buf, int bufsize) {
    if (bufsize < 8) { buf[0] = '\0'; return; }
    int i = 0;
    buf[i++] = (flags & PTE_WRITABLE) ? 'W' : 'R';
    buf[i++] = '-';
    buf[i++] = (flags & PTE_USER)    ? 'U' : 'K';
    buf[i++] = (flags & PTE_NX)      ? '-' : 'X';
    buf[i++] = (flags & PTE_GLOBAL)  ? 'G' : '-';
    buf[i++] = (flags & PTE_PWT)     ? 'T' : '-';  /* Write-Through */
    buf[i++] = (flags & PTE_PCD)     ? 'C' : '-';  /* Cache Disabled */
    buf[i]   = '\0';
}

/** Validate that a virtual address is page-aligned and canonical. */
static vmm_status_t validate_vaddr(uint64_t vaddr) {
    if (!is_page_aligned(vaddr)) return VMM_ERR_INVALID;
    if (!va_is_canonical(vaddr))  return VMM_ERR_INVALID;
    if (vaddr == 0)               return VMM_ERR_INVALID;
    return VMM_OK;
}

/** Validate that a physical address is page-aligned and non-zero. */
static vmm_status_t validate_paddr(uint64_t paddr) {
    if (!is_page_aligned(paddr)) return VMM_ERR_INVALID;
    if (paddr == 0)              return VMM_ERR_INVALID;
    return VMM_OK;
}

/** Validate an address space pointer. */
static vmm_status_t validate_aspace(address_space_t *as) {
    if (!as)          return VMM_ERR_INVALID;
    if (!as->pml4)    return VMM_ERR_INVALID;
    return VMM_OK;
}

/* =========================================================================
 * Init
 * ========================================================================= */

void vmm_init(void *serial_dev) {
    vmm_log_init(serial_dev);
    page_table_set_serial(serial_dev);

    VMM_LOG_INF("============================================\r\n");
    VMM_LOG_INF("Virtual Memory Manager initializing...\r\n");

    /* Capture the current CR3 — this is the boot page table PML4. */
    uint64_t cr3_phys = pt_read_cr3();

    g_kernel_aspace.pml4      = pt_phys_to_virt(cr3_phys);
    g_kernel_aspace.pml4_phys = cr3_phys;
    g_kernel_aspace.is_kernel = true;

    VMM_LOG_INF("Kernel PML4 @ ");
    vmm_log_hex64(cr3_phys);
    VMM_LOG_INF("\r\n");

    VMM_LOG_INF("Page size       : 4096 bytes\r\n");
    VMM_LOG_INF("Max virtual addr: 0xFFFFFFFFFFFFFFFF\r\n");

    g_vmm_initialized = true;

    VMM_LOG_INF("Init complete.\r\n");
    VMM_LOG_INF("============================================\r\n");
}

/* =========================================================================
 * Address Space Management
 * ========================================================================= */

vmm_status_t vmm_create_address_space(address_space_t *out) {
    if (!g_vmm_initialized) return VMM_ERR_NOT_INIT;
    if (!out)               return VMM_ERR_INVALID;

    VMM_LOG_INF("CREATE: allocating new PML4...\r\n");

    /* Allocate a fresh PML4 page. */
    uint64_t pml4_phys = pmm_alloc_frame();
    if (pml4_phys == 0) {
        VMM_LOG_ERR("CREATE: OOM — cannot allocate PML4\r\n");
        return VMM_ERR_NO_MEM;
    }

    uint64_t *pml4 = pt_phys_to_virt(pml4_phys);

    /* Zero the entire PML4. */
    memset(pml4, 0, PAGE_SIZE);

    /* Zero the entire PML4 (done above) — the user half (entries 0-255)
     * starts empty and is populated only by user mappings, which install
     * the USER bit on every level of the walk.  We deliberately do NOT
     * copy kernel PML4[0] here: that entry is user space (the lowest
     * 512 GiB), and copying it can inherit a supervisor-only or dangling
     * entry that makes every user access to the low half fault. */

    /* Copy upper 256 entries (kernel half) from the kernel PML4.
     * This ensures the kernel is mapped in every address space. */
    for (int i = 256; i < PT_ENTRIES; i++) {
        pml4[i] = g_kernel_aspace.pml4[i];
    }

    out->pml4      = pml4;
    out->pml4_phys = pml4_phys;
    out->is_kernel = false;

    VMM_LOG_INF("CREATE: PML4 allocated @ ");
    vmm_log_hex64(pml4_phys);
    VMM_LOG_INF(" (kernel entries copied)\r\n");

    return VMM_OK;
}

vmm_status_t vmm_destroy_address_space(address_space_t *as) {
    if (!g_vmm_initialized)  return VMM_ERR_NOT_INIT;
    if (!as)                 return VMM_ERR_INVALID;
    if (!as->pml4)           return VMM_ERR_INVALID;
    if (as->is_kernel) {
        VMM_LOG_WRN("DESTROY: cannot destroy kernel address space\r\n");
        return VMM_ERR_INVALID;
    }

    VMM_LOG_INF("DESTROY: freeing user page tables and frames...\r\n");

    /* Walk all user PML4 entries (0-255) and free leaf frames + page tables.
     * Skip entries shared with the kernel address space. */
    address_space_t *kspace = vmm_get_kernel_address_space();
    for (int i = 0; i < 256; i++) {
        uint64_t entry = as->pml4[i];
        if (!(entry & PTE_PRESENT)) continue;

        /* Skip entries shared with the kernel address space. */
        if (kspace && kspace->pml4 && entry == kspace->pml4[i]) continue;

        uint64_t pdpt_phys = pte_addr(entry);
        uint64_t *pdpt = pt_phys_to_virt(pdpt_phys);

        /* Walk PDPT entries. */
        for (int j = 0; j < PT_ENTRIES; j++) {
            uint64_t pdpe = pdpt[j];
            if (!(pdpe & PTE_PRESENT)) continue;

            if (pdpe & PTE_PS) {
                /* 1 GiB huge page — free the frame. */
                pmm_free_frame(pte_addr(pdpe));
                continue;
            }

            uint64_t pd_phys = pte_addr(pdpe);
            uint64_t *pd = pt_phys_to_virt(pd_phys);

            /* Walk PD entries. */
            for (int k = 0; k < PT_ENTRIES; k++) {
                uint64_t pde = pd[k];
                if (!(pde & PTE_PRESENT)) continue;

                if (pde & PTE_PS) {
                    /* 2 MiB huge page — free the frame. */
                    pmm_free_frame(pte_addr(pde));
                    continue;
                }

                uint64_t pt_phys = pte_addr(pde);
                uint64_t *pt = pt_phys_to_virt(pt_phys);

                /* Walk PT entries — these are leaf PTEs. */
                for (int m = 0; m < PT_ENTRIES; m++) {
                    uint64_t pte = pt[m];
                    if (!(pte & PTE_PRESENT)) {
                        /* Demand PTEs have no physical frame — just skip. */
                        continue;
                    }

                    /* Free the physical frame mapped by this leaf PTE. */
                    pmm_free_frame(pte_addr(pte));
                }

                /* Free the PT page itself. */
                pmm_free_frame(pt_phys);
            }

            /* Free the PD page itself. */
            pmm_free_frame(pd_phys);
        }

        /* Free the PDPT page itself. */
        pmm_free_frame(pdpt_phys);
        as->pml4[i] = 0;
    }

    /* Free the PML4 page itself. */
    uint64_t pml4_phys = as->pml4_phys;
    as->pml4 = NULL;
    as->pml4_phys = 0;
    pmm_free_frame(pml4_phys);

    VMM_LOG_INF("DESTROY: address space freed (frames + tables released)\r\n");
    return VMM_OK;
}

/* =========================================================================
 * Page Mapping
 * ========================================================================= */

vmm_status_t vmm_map_page(address_space_t *as, uint64_t vaddr,
                          uint64_t paddr, uint64_t flags) {
    if (!g_vmm_initialized) return VMM_ERR_NOT_INIT;

    vmm_status_t st;

    /* Validate inputs. */
    st = validate_aspace(as);
    if (st != VMM_OK) return st;

    st = validate_vaddr(vaddr);
    if (st != VMM_OK) return st;

    st = validate_paddr(paddr);
    if (st != VMM_OK) return st;

    /* Ensure PRESENT is always set on the leaf.  Intermediate entries
     * use PT_DEFAULT_FLAGS (PRESENT | WRITABLE) plus the USER bit when
     * the leaf is a user mapping — user access requires USER=1 on every
     * level of the walk, not just the PTE. */
    uint64_t leaf_flags = flags | PTE_PRESENT;
    uint64_t inter_flags = PT_DEFAULT_FLAGS | (flags & PTE_USER);

    /* Walk to the leaf PTE, allocating intermediate tables. */
    uint64_t *pte = pt_walk_to_leaf(as->pml4, vaddr, true,
                                     inter_flags);
    if (!pte) {
        VMM_LOG_ERR("MAP: walk failed for vaddr ");
        vmm_log_hex64(vaddr);
        VMM_LOG_ERR("\r\n");
        return VMM_ERR_NO_MEM;
    }

    /* Check for existing mapping. */
    if (*pte & PTE_PRESENT) {
        VMM_LOG_WRN("MAP: vaddr ");
        vmm_log_hex64(vaddr);
        VMM_LOG_WRN(" already mapped to ");
        vmm_log_hex64(pte_addr(*pte));
        VMM_LOG_WRN("\r\n");
        return VMM_ERR_ALREADY_MAP;
    }

    /* Install the leaf PTE. */
    uint64_t new_pte = pte_make(paddr, leaf_flags);
    *pte = new_pte;

    /* Flush TLB for this address. */
    pt_invlpg((void *)vaddr);

    /* Log the mapping. */
    char fbuf[8];
    format_flags(leaf_flags, fbuf, sizeof(fbuf));
    VMM_LOG_INF("MAP: vaddr ");
    vmm_log_hex64(vaddr);
    VMM_LOG_INF(" -> paddr ");
    vmm_log_hex64(paddr);
    VMM_LOG_INF(" [");
    vmm_log_write(1, fbuf);
    VMM_LOG_INF("]\r\n");

    return VMM_OK;
}

vmm_status_t vmm_map_page_merge(address_space_t *as, uint64_t vaddr,
                                uint64_t paddr, uint64_t flags) {
    if (!g_vmm_initialized) return VMM_ERR_NOT_INIT;

    vmm_status_t st;

    /* Validate inputs. */
    st = validate_aspace(as);
    if (st != VMM_OK) return st;

    st = validate_vaddr(vaddr);
    if (st != VMM_OK) return st;

    st = validate_paddr(paddr);
    if (st != VMM_OK) return st;

    /* Ensure PRESENT is always set on the leaf.  Intermediate entries
     * use PT_DEFAULT_FLAGS (PRESENT | WRITABLE) plus the USER bit when
     * the leaf is a user mapping — user access requires USER=1 on every
     * level of the walk, not just the PTE. */
    uint64_t leaf_flags = flags | PTE_PRESENT;
    uint64_t inter_flags = PT_DEFAULT_FLAGS | (flags & PTE_USER);

    /* Walk to the leaf PTE, allocating intermediate tables. */
    uint64_t *pte = pt_walk_to_leaf(as->pml4, vaddr, true,
                                     inter_flags);
    if (!pte) {
        VMM_LOG_ERR("MAPM: walk failed for vaddr ");
        vmm_log_hex64(vaddr);
        VMM_LOG_ERR("\r\n");
        return VMM_ERR_NO_MEM;
    }

    /* Existing mapping: upgrade permissions in place (keep the paddr
     * already installed; flag bits are OR'd together).  This is what
     * allows ELF PT_LOAD segments that share a page (e.g. a read-exec
     * text page tail also covered by a read-write data segment) to
     * map cleanly without failing.  NX is dropped whenever either
     * segment allows execution. */
    if (*pte & PTE_PRESENT) {
        uint64_t merged_flags = (*pte | leaf_flags);
        if (!(leaf_flags & PTE_NX)) merged_flags &= ~PTE_NX;
        uint64_t merged = (*pte & PTE_ADDR_MASK) | merged_flags;
        if (merged != *pte) {
            *pte = merged;
            pt_invlpg((void *)vaddr);

            char fbuf[8];
            format_flags(merged_flags, fbuf, sizeof(fbuf));
            VMM_LOG_INF("MAPM: vaddr ");
            vmm_log_hex64(vaddr);
            VMM_LOG_INF(" flags upgraded [");
            vmm_log_write(1, fbuf);
            VMM_LOG_INF("]\r\n");
        }
        return VMM_OK;
    }

    /* Fresh mapping: install the leaf PTE. */
    uint64_t new_pte = pte_make(paddr, leaf_flags);
    *pte = new_pte;

    /* Flush TLB for this address. */
    pt_invlpg((void *)vaddr);

    /* Log the mapping. */
    char fbuf[8];
    format_flags(leaf_flags, fbuf, sizeof(fbuf));
    VMM_LOG_INF("MAPM: vaddr ");
    vmm_log_hex64(vaddr);
    VMM_LOG_INF(" -> paddr ");
    vmm_log_hex64(paddr);
    VMM_LOG_INF(" [");
    vmm_log_write(1, fbuf);
    VMM_LOG_INF("]\r\n");

    return VMM_OK;
}

vmm_status_t vmm_unmap_page(address_space_t *as, uint64_t vaddr) {
    if (!g_vmm_initialized) return VMM_ERR_NOT_INIT;

    vmm_status_t st;

    st = validate_aspace(as);
    if (st != VMM_OK) return st;

    st = validate_vaddr(vaddr);
    if (st != VMM_OK) return st;

    /* Walk to the leaf PTE (don't create). */
    uint64_t *pte = pt_walk_to_leaf(as->pml4, vaddr, false, 0);
    if (!pte || !(*pte & (PTE_PRESENT | PTE_DEMAND))) {
        VMM_LOG_WRN("UNMAP: vaddr ");
        vmm_log_hex64(vaddr);
        VMM_LOG_WRN(" not mapped\r\n");
        return VMM_ERR_NOT_MAPPED;
    }

    /* Save old PTE for logging. */
    uint64_t old_pte = *pte;

    /* Clear the PTE. */
    *pte = 0;

    /* Flush TLB. */
    pt_invlpg((void *)vaddr);

    VMM_LOG_INF("UNMAP: vaddr ");
    vmm_log_hex64(vaddr);
    VMM_LOG_INF(" (was paddr ");
    vmm_log_hex64(pte_addr(old_pte));
    VMM_LOG_INF(")\r\n");

    /* Free intermediate page tables that became empty after this unmap.
     * Only user-space entries (PML4 index 0-255) may be freed.
     * Note: demand PTEs (no physical frame) do NOT call pmm_free_frame. */
    pt_cleanup_empty_tables(as->pml4, vaddr);

    return VMM_OK;
}

vmm_status_t vmm_unmap_range(address_space_t *as, uint64_t vaddr_start,
                             uint64_t vaddr_end) {
    if (!g_vmm_initialized) return VMM_ERR_NOT_INIT;

    vmm_status_t st;
    st = validate_aspace(as);
    if (st != VMM_OK) return st;

    if (vaddr_start >= vaddr_end) return VMM_ERR_INVALID;
    if (!is_page_aligned(vaddr_start) || !is_page_aligned(vaddr_end)) {
        return VMM_ERR_INVALID;
    }

    VMM_LOG_INF("UNMAP_RANGE: [");
    vmm_log_hex64(vaddr_start);
    VMM_LOG_INF(" - ");
    vmm_log_hex64(vaddr_end);
    VMM_LOG_INF(")\r\n");

    for (uint64_t va = vaddr_start; va < vaddr_end; va += PAGE_SIZE) {
        vmm_unmap_page(as, va);
        /* Guard: prevent wrap-around at the top of the 64-bit address space.
         * If va + PAGE_SIZE would overflow, we have unmapped the last page. */
        if (va + PAGE_SIZE < va) break;
    }

    return VMM_OK;
}

/* =========================================================================
 * Translation
 * ========================================================================= */

uint64_t vmm_translate(address_space_t *as, uint64_t vaddr) {
    if (!g_vmm_initialized) return 0;
    if (!as || !as->pml4)   return 0;

    uint64_t offset = vaddr & 0xFFF;

    /* Walk PML4. */
    uint64_t pml4e = pt_get_entry(as->pml4, PML4_INDEX(vaddr));
    if (!(pml4e & PTE_PRESENT)) return 0;

    /* Walk PDPT. */
    uint64_t *pdpt = (uint64_t *)pt_phys_to_virt(pte_addr(pml4e));
    uint64_t pdpe = pdpt[PDPT_INDEX(vaddr)];
    if (!(pdpe & PTE_PRESENT)) return 0;

    /* 1 GiB huge page. */
    if (pdpe & PTE_PS) {
        return (pdpe & 0x000FFFFFC0000000ULL) + (vaddr & 0x3FFFFFFFULL);
    }

    /* Walk PD. */
    uint64_t *pd = (uint64_t *)pt_phys_to_virt(pte_addr(pdpe));
    uint64_t pde = pd[PD_INDEX(vaddr)];
    if (!(pde & PTE_PRESENT)) return 0;

    /* 2 MiB huge page. */
    if (pde & PTE_PS) {
        return (pde & 0x000FFFFFFFE00000ULL) + (vaddr & 0x1FFFFFULL);
    }

    /* Walk PT. */
    uint64_t *pt = (uint64_t *)pt_phys_to_virt(pte_addr(pde));
    uint64_t pte = pt[PT_INDEX(vaddr)];
    if (!(pte & PTE_PRESENT)) return 0;

    return pte_addr(pte) + offset;
}

/* =========================================================================
 * Clone
 * ========================================================================= */

vmm_status_t vmm_clone_address_space(address_space_t *src,
                                     address_space_t *dst) {
    if (!g_vmm_initialized) return VMM_ERR_NOT_INIT;
    if (!src || !dst)       return VMM_ERR_INVALID;
    if (!src->pml4)         return VMM_ERR_INVALID;
    if (!dst->pml4)         return VMM_ERR_INVALID;

    VMM_LOG_INF("CLONE: src PML4=");
    vmm_log_hex64(src->pml4_phys);
    VMM_LOG_INF(" -> dst PML4=");
    vmm_log_hex64(dst->pml4_phys);
    VMM_LOG_INF("\r\n");

    /* dst already has kernel mappings (from vmm_create_address_space).
     * We only need to clone user-space entries (indices 0-255). */
    for (int i = 0; i < 256; i++) {
        uint64_t src_entry = src->pml4[i];
        if (!(src_entry & PTE_PRESENT)) continue;

        /* Allocate a new PDPT for the destination. */
        uint64_t new_pdpt_phys = pmm_alloc_frame();
        if (new_pdpt_phys == 0) {
            VMM_LOG_ERR("CLONE: OOM at PML4[");
            vmm_log_uint64((uint64_t)i);
            VMM_LOG_ERR("]\r\n");
            /* Free ALL previously-installed user PML4 entries (0..i-1). */
            for (int c = 0; c < i; c++) {
                if (!(dst->pml4[c] & PTE_PRESENT)) continue;
                uint64_t pdpt_phys_c = pte_addr(dst->pml4[c]);
                uint64_t *pdpt_c = pt_phys_to_virt(pdpt_phys_c);
                pt_free_recursive(pdpt_c, 2);
                pmm_free_frame(pdpt_phys_c);
                dst->pml4[c] = 0;
            }
            return VMM_ERR_NO_MEM;
        }

        uint64_t *src_pdpt = (uint64_t *)
            pt_phys_to_virt(pte_addr(src_entry));
        uint64_t *dst_pdpt = pt_phys_to_virt(new_pdpt_phys);
        memset(dst_pdpt, 0, PAGE_SIZE);

        /* Copy and recurse: clone PD and PT tables. */
        for (int j = 0; j < PT_ENTRIES; j++) {
            uint64_t src_pdpe = src_pdpt[j];
            if (!(src_pdpe & PTE_PRESENT)) {
                dst_pdpt[j] = 0;
                continue;
            }

            /* Huge page at PDPT level — copy directly. */
            if (src_pdpe & PTE_PS) {
                dst_pdpt[j] = src_pdpe;
                continue;
            }

            /* Allocate new PD, copy entries. */
            uint64_t new_pd_phys = pmm_alloc_frame();
            if (new_pd_phys == 0) {
                VMM_LOG_ERR("CLONE: OOM at PDPT\r\n");
                pt_free_recursive(dst_pdpt, 2);
                pmm_free_frame(new_pdpt_phys);
                /* Free ALL previously-installed user PML4 entries. */
                for (int c = 0; c < i; c++) {
                    if (!(dst->pml4[c] & PTE_PRESENT)) continue;
                    uint64_t pdpt_phys_c = pte_addr(dst->pml4[c]);
                    uint64_t *pdpt_c = pt_phys_to_virt(pdpt_phys_c);
                    pt_free_recursive(pdpt_c, 2);
                    pmm_free_frame(pdpt_phys_c);
                    dst->pml4[c] = 0;
                }
                return VMM_ERR_NO_MEM;
            }

            uint64_t *src_pd = (uint64_t *)
                pt_phys_to_virt(pte_addr(src_pdpe));
            uint64_t *dst_pd = pt_phys_to_virt(new_pd_phys);
            memset(dst_pd, 0, PAGE_SIZE);

            for (int k = 0; k < PT_ENTRIES; k++) {
                uint64_t src_pde = src_pd[k];
                if (!(src_pde & PTE_PRESENT)) {
                    dst_pd[k] = 0;
                    continue;
                }

                /* Huge page at PD level — copy directly. */
                if (src_pde & PTE_PS) {
                    dst_pd[k] = src_pde;
                    continue;
                }

                /* Allocate new PT, copy 512 entries with COW marking. */
                uint64_t new_pt_phys = pmm_alloc_frame();
                if (new_pt_phys == 0) {
                    VMM_LOG_ERR("CLONE: OOM at PD\r\n");
                    /* Clean up current PD subtree (entries k..511 already 0). */
                    pt_free_recursive(dst_pd, 1);
                    pmm_free_frame(new_pd_phys);
                    dst_pdpt[j] = 0;
                    pt_free_recursive(dst_pdpt, 2);
                    pmm_free_frame(new_pdpt_phys);
                    /* Free ALL previously-installed user PML4 entries. */
                    for (int c = 0; c < i; c++) {
                        if (!(dst->pml4[c] & PTE_PRESENT)) continue;
                        uint64_t pdpt_phys_c = pte_addr(dst->pml4[c]);
                        uint64_t *pdpt_c = pt_phys_to_virt(pdpt_phys_c);
                        pt_free_recursive(pdpt_c, 2);
                        pmm_free_frame(pdpt_phys_c);
                        dst->pml4[c] = 0;
                    }
                    return VMM_ERR_NO_MEM;
                }

                uint64_t *src_pt = (uint64_t *)
                    pt_phys_to_virt(pte_addr(src_pde));
                uint64_t *dst_pt = (uint64_t *)
                    pt_phys_to_virt(new_pt_phys);

                for (int m = 0; m < PT_ENTRIES; m++) {
                    uint64_t src_leaf = src_pt[m];

                    /* Copy demand PTEs as-is — no physical frame to share. */
                    if ((src_leaf & PTE_DEMAND) && !(src_leaf & PTE_PRESENT)) {
                        dst_pt[m] = src_leaf;
                        continue;
                    }

                    if (!(src_leaf & PTE_PRESENT)) {
                        dst_pt[m] = 0;
                        continue;
                    }

                    uint64_t leaf_phys = pte_addr(src_leaf);
                    uint64_t leaf_flags = pte_flags(src_leaf);

                    /* COW: mark writable pages that aren't already COW. */
                    if ((leaf_flags & PTE_WRITABLE) &&
                        !(leaf_flags & PTE_COW)) {
                        uint64_t cow_flags = (leaf_flags & ~PTE_WRITABLE)
                                           | PTE_COW;
                        src_pt[m] = pte_make(leaf_phys, cow_flags);
                        dst_pt[m] = pte_make(leaf_phys, cow_flags);
                    } else {
                        /* Read-only or already COW — copy as-is. */
                        dst_pt[m] = src_leaf;
                    }
                }

                dst_pd[k] = pte_make(new_pt_phys, pte_flags(src_pde));
            }

            dst_pdpt[j] = pte_make(new_pd_phys, pte_flags(src_pdpe));
        }

        /* Install the cloned PDPT into the destination PML4. */
        dst->pml4[i] = pte_make(new_pdpt_phys, pte_flags(src_entry));
    }

    VMM_LOG_INF("CLONE: complete\r\n");
    return VMM_OK;
}

/* =========================================================================
 * Address Space Switch
 * ========================================================================= */

void vmm_switch_address_space(address_space_t *as) {
    if (!as || !as->pml4_phys) return;

    VMM_LOG_DBG("SWITCH: loading CR3 = ");
    vmm_log_hex64(as->pml4_phys);
    VMM_LOG_DBG("\r\n");

    pt_write_cr3(as->pml4_phys);
}

address_space_t *vmm_get_kernel_address_space(void) {
    if (!g_vmm_initialized) return NULL;
    return &g_kernel_aspace;
}

bool vmm_is_initialized(void) {
    return g_vmm_initialized;
}

vmm_status_t vmm_unmap_and_free(address_space_t *as, uint64_t vaddr) {
    if (!g_vmm_initialized) return VMM_ERR_NOT_INIT;

    vmm_status_t st;
    st = validate_aspace(as);
    if (st != VMM_OK) return st;

    st = validate_vaddr(vaddr);
    if (st != VMM_OK) return st;

    /* Walk to the leaf PTE. */
    uint64_t *pte = pt_walk_to_leaf(as->pml4, vaddr, false, 0);
    if (!pte) {
        return VMM_ERR_NOT_MAPPED;
    }

    uint64_t old_pte_val = *pte;

    /* Handle demand PTEs (no physical frame allocated). */
    if (old_pte_val & PTE_DEMAND) {
        *pte = 0;
        pt_invlpg((void *)vaddr);
        pt_cleanup_empty_tables(as->pml4, vaddr);
        return VMM_OK;
    }

    if (!(old_pte_val & PTE_PRESENT)) {
        return VMM_ERR_NOT_MAPPED;
    }

    /* Save physical address before clearing. */
    uint64_t phys = pte_addr(old_pte_val);

    /* Clear the PTE and flush TLB. */
    *pte = 0;
    pt_invlpg((void *)vaddr);

    /* Free the physical frame back to PMM. */
    pmm_free_frame(phys);

    VMM_LOG_INF("UNMAP_FREE: vaddr ");
    vmm_log_hex64(vaddr);
    VMM_LOG_INF(" -> paddr ");
    vmm_log_hex64(phys);
    VMM_LOG_INF(" freed\r\n");

    /* Free intermediate page tables that became empty. */
    pt_cleanup_empty_tables(as->pml4, vaddr);

    return VMM_OK;
}

/* =========================================================================
 * Demand-Page Mapping (Lazy Allocation)
 * ========================================================================= */

vmm_status_t vmm_map_demand_page(address_space_t *as, uint64_t vaddr,
                                 uint64_t flags) {
    if (!g_vmm_initialized) return VMM_ERR_NOT_INIT;

    vmm_status_t st;
    st = validate_aspace(as);
    if (st != VMM_OK) return st;

    st = validate_vaddr(vaddr);
    if (st != VMM_OK) return st;

    /* Walk to the leaf PTE, creating intermediate tables as needed.
     * Intermediate entries use PRESENT | WRITABLE (same as vmm_map_page)
     * plus the USER bit when the leaf is a user mapping — user access
     * requires USER=1 on every level of the walk. */
    uint64_t inter_flags = PT_DEFAULT_FLAGS | (flags & PTE_USER);
    uint64_t *pte = pt_walk_to_leaf(as->pml4, vaddr, true,
                                     inter_flags);
    if (!pte) {
        VMM_LOG_ERR("DEMAND: walk failed for vaddr ");
        vmm_log_hex64(vaddr);
        VMM_LOG_ERR("\r\n");
        return VMM_ERR_NO_MEM;
    }

    /* If already mapped (present or demand), don't overwrite. */
    if (*pte & (PTE_PRESENT | PTE_DEMAND)) {
        return VMM_ERR_ALREADY_MAP;
    }

    /* Install a non-present PTE with PTE_DEMAND set.
     * The page fault handler will allocate a frame on first access. */
    uint64_t demand_flags = (flags & ~PTE_ADDR_MASK) | PTE_DEMAND;
    /* Explicitly clear PRESENT — this is a demand page. */
    demand_flags &= ~PTE_PRESENT;
    *pte = demand_flags;  /* No physical address — address field is zero. */

    pt_invlpg((void *)vaddr);

    VMM_LOG_DBG("DEMAND: mapped vaddr ");
    vmm_log_hex64(vaddr);
    VMM_LOG_DBG(" flags=");
    {
        char fbuf[8];
        format_flags(demand_flags, fbuf, sizeof(fbuf));
        VMM_LOG_DBG("[");
        vmm_log_write(1, fbuf);
        VMM_LOG_DBG("]\r\n");
    }

    return VMM_OK;
}
