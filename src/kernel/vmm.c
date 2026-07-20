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

    g_kernel_aspace.pml4      = (uint64_t *)(uintptr_t)cr3_phys;  /* identity map */
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

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;  /* identity map */

    /* Zero the entire PML4. */
    memset(pml4, 0, PAGE_SIZE);

    /* Copy entry 0 (identity mapping) from the kernel PML4 so the
     * kernel code at physical addresses remains accessible after CR3
     * switch.  The PDPT and PD pages are shared (not copied). */
    pml4[0] = g_kernel_aspace.pml4[0];

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

    VMM_LOG_INF("DESTROY: freeing user page tables...\r\n");

    /* Free all user-mode page table pages (levels 1, 2, 3).
     * Level 3 = PDPT sub-tables (256 user entries in PML4).
     * We recurse from each user PDPT entry.
     * Skip entry 0 — it is the shared kernel identity mapping. */
    for (int i = 1; i < 256; i++) {
        uint64_t entry = as->pml4[i];
        if (!(entry & PTE_PRESENT)) continue;

        /* The PDPT page itself is at pte_addr(entry).
         * Recurse to free PD and PT pages under it. */
        uint64_t pdpt_phys = pte_addr(entry);
        uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;

        pt_free_recursive(pdpt, 2);  /* level 2 = free PD and PT pages */

        /* Free the PDPT page itself. */
        pmm_free_frame(pdpt_phys);
        as->pml4[i] = 0;
    }

    /* Free the PML4 page itself. */
    uint64_t pml4_phys = as->pml4_phys;
    as->pml4 = NULL;
    as->pml4_phys = 0;
    pmm_free_frame(pml4_phys);

    VMM_LOG_INF("DESTROY: address space freed\r\n");
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
     * use PT_DEFAULT_FLAGS (PRESENT | WRITABLE) for the walk. */
    uint64_t leaf_flags = flags | PTE_PRESENT;

    /* Walk to the leaf PTE, allocating intermediate tables. */
    uint64_t *pte = pt_walk_to_leaf(as->pml4, vaddr, true,
                                     PT_DEFAULT_FLAGS);
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

vmm_status_t vmm_unmap_page(address_space_t *as, uint64_t vaddr) {
    if (!g_vmm_initialized) return VMM_ERR_NOT_INIT;

    vmm_status_t st;

    st = validate_aspace(as);
    if (st != VMM_OK) return st;

    st = validate_vaddr(vaddr);
    if (st != VMM_OK) return st;

    /* Walk to the leaf PTE (don't create). */
    uint64_t *pte = pt_walk_to_leaf(as->pml4, vaddr, false, 0);
    if (!pte || !(*pte & PTE_PRESENT)) {
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
     * Only user-space entries (PML4 index 1-255) may be freed. */
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
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pml4e));
    uint64_t pdpe = pdpt[PDPT_INDEX(vaddr)];
    if (!(pdpe & PTE_PRESENT)) return 0;

    /* 1 GiB huge page. */
    if (pdpe & PTE_PS) {
        return (pdpe & 0x000FFFFFC0000000ULL) + (vaddr & 0x3FFFFFFFULL);
    }

    /* Walk PD. */
    uint64_t *pd = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pdpe));
    uint64_t pde = pd[PD_INDEX(vaddr)];
    if (!(pde & PTE_PRESENT)) return 0;

    /* 2 MiB huge page. */
    if (pde & PTE_PS) {
        return (pde & 0x000FFFFFFFE00000ULL) + (vaddr & 0x1FFFFFULL);
    }

    /* Walk PT. */
    uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pde));
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
            return VMM_ERR_NO_MEM;
        }

        uint64_t *src_pdpt = (uint64_t *)(uintptr_t)
            pt_phys_to_virt(pte_addr(src_entry));
        uint64_t *dst_pdpt = (uint64_t *)(uintptr_t)new_pdpt_phys;
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
                return VMM_ERR_NO_MEM;
            }

            uint64_t *src_pd = (uint64_t *)(uintptr_t)
                pt_phys_to_virt(pte_addr(src_pdpe));
            uint64_t *dst_pd = (uint64_t *)(uintptr_t)new_pd_phys;
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

                /* Allocate new PT, copy 512 entries with COW marking.
                 * Writable user pages get COW flag set and WRITABLE
                 * cleared in both src and dst — first write triggers
                 * a page fault that allocates a private copy. */
                uint64_t new_pt_phys = pmm_alloc_frame();
                if (new_pt_phys == 0) {
                    VMM_LOG_ERR("CLONE: OOM at PD\r\n");
                    pt_free_recursive(dst_pd, 1);
                    pmm_free_frame(new_pd_phys);
                    pt_free_recursive(dst_pdpt, 2);
                    pmm_free_frame(new_pdpt_phys);
                    return VMM_ERR_NO_MEM;
                }

                uint64_t *src_pt = (uint64_t *)(uintptr_t)
                    pt_phys_to_virt(pte_addr(src_pde));
                uint64_t *dst_pt = (uint64_t *)(uintptr_t)
                    pt_phys_to_virt(new_pt_phys);

                for (int m = 0; m < PT_ENTRIES; m++) {
                    uint64_t src_leaf = src_pt[m];
                    if (!(src_leaf & PTE_PRESENT)) {
                        dst_pt[m] = 0;
                        continue;
                    }

                    uint64_t leaf_phys = pte_addr(src_leaf);
                    uint64_t leaf_flags = pte_flags(src_leaf);

                    /* COW: mark writable pages that aren't already COW.
                     * Clear WRITABLE in both src and dst; first write
                     * triggers #PF → handler allocates private copy. */
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

        /* Install the cloned PDPT into the destination PML4.
         * Preserve the same flags as the source. */
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
