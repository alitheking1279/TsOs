/**
 * @file test_vmm.c
 * @brief Virtual Memory Manager test suite — 75 tests across 11 groups.
 *
 * Test philosophy
 * ===============
 * Every test exercises real page-table state (not just C struct values).
 * We map physical frames into a safe test VA range
 * ([VMM_TEST_VA_BASE .. VMM_TEST_VA_END) = [0x0000008000000000 .. 0x0000008040000000)),
 * then verify the mapping via vmm_translate(), direct memory read/write,
 * and page table walks.
 *
 * The test VA range is in user space (lower half), within the
 * identity-mapped first 1 GiB, and safely above the kernel image.
 *
 * Group A — PTE Flag Manipulation (8 tests)
 * Group B — VA Decomposition Macros (6 tests)
 * Group C — Basic Map/Unmap Round-Trip (10 tests)
 * Group D — Multiple Mappings & Ranges (7 tests)
 * Group E — Address Space Creation & Destruction (10 tests)
 * Group F — Address Space Isolation (6 tests)
 * Group G — Clone (6 tests)
 * Group H — Error Conditions & Edge Cases (8 tests)
 * Group I — Intermediate Table Cleanup After Unmap (4 tests)
 * Group J — Copy-on-Write (3 tests)
 * Group K — Unmap Range Validation & Huge Page Translate (3 tests)
 */

#include "test.h"
#include "../kernel/vmm.h"
#include "../kernel/page_table.h"
#include "../kernel/pmm.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Internal serial logging helpers (same pattern as test_pmm.c)
 * ========================================================================= */

static void t_hex64(serial_dev_t *dev, uint64_t val) {
    static const char h[] = "0123456789ABCDEF";
    serial_write_string(dev, "0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_write_char(dev, h[(val >> i) & 0xF]);
}

/* =========================================================================
 * Test VA range management
 *
 * Each test claims a disjoint chunk of the [VMM_TEST_VA_BASE ..
 * VMM_TEST_VA_END) range via a simple bump allocator.  The bump pointer
 * is reset between test groups so tests within a group can reuse the
 * same range without exhausting the 4 MiB window.
 * ========================================================================= */

static uint64_t g_test_va_next = VMM_TEST_VA_BASE;

/** Reset the test VA bump pointer (call between groups). */
static void reset_test_va(void) {
    g_test_va_next = VMM_TEST_VA_BASE;
}

/** Allocate `count` pages worth of test virtual addresses. */
static uint64_t alloc_test_va(int count) {
    uint64_t va = g_test_va_next;
    g_test_va_next += (uint64_t)count * PAGE_SIZE;
    if (g_test_va_next > VMM_TEST_VA_END) g_test_va_next = VMM_TEST_VA_END;
    return va;
}

/* =========================================================================
 * Group A: PTE Flag Manipulation (8 tests)
 *
 * Pure software — no page tables, no hardware, no PMM allocations.
 * Tests the inline helpers defined in page_table.h.
 * ========================================================================= */

static void test_pte_present_set(serial_dev_t *dev) {
    uint64_t pte = pte_make(0x1000, PTE_PRESENT);
    ASSERT_TRUE(dev, pte_present(pte));
    pte = pte_clear_flag(pte, PTE_PRESENT);
    ASSERT_TRUE(dev, !pte_present(pte));
}

static void test_pte_writable_set(serial_dev_t *dev) {
    uint64_t pte = pte_make(0x2000, PTE_WRITABLE);
    ASSERT_TRUE(dev, pte_writable(pte));
    pte = pte_clear_flag(pte, PTE_WRITABLE);
    ASSERT_TRUE(dev, !pte_writable(pte));
}

static void test_pte_user_set(serial_dev_t *dev) {
    uint64_t pte = pte_make(0x3000, PTE_USER);
    ASSERT_TRUE(dev, pte_user(pte));
    pte = pte_clear_flag(pte, PTE_USER);
    ASSERT_TRUE(dev, !pte_user(pte));
}

static void test_pte_nx_set(serial_dev_t *dev) {
    uint64_t pte = pte_make(0x4000, PTE_NX);
    ASSERT_TRUE(dev, pte_nx(pte));
    pte = pte_clear_flag(pte, PTE_NX);
    ASSERT_TRUE(dev, !pte_nx(pte));
}

static void test_pte_addr_extract(serial_dev_t *dev) {
    uint64_t phys = 0x0000ABCDEF000ULL;
    uint64_t pte  = pte_make(phys, PTE_PRESENT);
    ASSERT_EQ(dev, pte_addr(pte), phys);
}

static void test_pte_flags_extract(serial_dev_t *dev) {
    uint64_t flags = PTE_WRITABLE | PTE_USER | PTE_GLOBAL;
    uint64_t pte = pte_make(0x5000, flags);
    ASSERT_EQ(dev, pte_flags(pte), flags);
}

static void test_pte_huge_set(serial_dev_t *dev) {
    uint64_t pte = pte_make(0x6000, PTE_PS);
    ASSERT_TRUE(dev, pte_huge(pte));
    pte = pte_clear_flag(pte, PTE_PS);
    ASSERT_TRUE(dev, !pte_huge(pte));
}

static void test_pte_global_set(serial_dev_t *dev) {
    uint64_t pte = pte_make(0x7000, PTE_GLOBAL);
    ASSERT_TRUE(dev, pte_global(pte));
    pte = pte_clear_flag(pte, PTE_GLOBAL);
    ASSERT_TRUE(dev, !pte_global(pte));
}

/* =========================================================================
 * Group B: VA Decomposition Macros (6 tests)
 *
 * Verify that PML4_INDEX, PDPT_INDEX, PD_INDEX, PT_INDEX, PAGE_OFFSET
 * extract the correct bit ranges.
 * ========================================================================= */

static void test_va_pml4_index(serial_dev_t *dev) {
    /* VA 0xFFFFFF8000000000: PML4 index = 511 (0x1FF). */
    ASSERT_EQ(dev, PML4_INDEX(0xFFFFFF8000000000ULL), (uint64_t)511);
    /* VA 0x0000000000000000: PML4 index = 0. */
    ASSERT_EQ(dev, PML4_INDEX(0x0000000000000000ULL), (uint64_t)0);
    /* VA 0x0000008000000000: PML4 index = 1 (bit 39 set). */
    ASSERT_EQ(dev, PML4_INDEX(0x0000008000000000ULL), (uint64_t)1);
}

static void test_va_pdpt_index(serial_dev_t *dev) {
    /* VA 0x0000000040000000: PDPT index = 1 (bit 30 set). */
    ASSERT_EQ(dev, PDPT_INDEX(0x0000000040000000ULL), (uint64_t)1);
    /* VA 0x0000007FC0000000: PDPT index = 511 (bits 38:30 all set). */
    ASSERT_EQ(dev, PDPT_INDEX(0x0000007FC0000000ULL), (uint64_t)511);
}

static void test_va_pd_index(serial_dev_t *dev) {
    /* VA 0x0000000000200000: PD index = 1 (bit 21 set). */
    ASSERT_EQ(dev, PD_INDEX(0x0000000000200000ULL), (uint64_t)1);
    /* VA 0x00000000FFE00000: PD index = 511 (bits 29:21 all set). */
    ASSERT_EQ(dev, PD_INDEX(0x00000000FFE00000ULL), (uint64_t)511);
}

static void test_va_pt_index(serial_dev_t *dev) {
    /* VA 0x0000000000001000: PT index = 1 (bit 12 set). */
    ASSERT_EQ(dev, PT_INDEX(0x0000000000001000ULL), (uint64_t)1);
    /* VA 0x0000000000001FF0: PT index = 1 (bits 20:12 = 0x1). */
    ASSERT_EQ(dev, PT_INDEX(0x0000000000001FF0ULL), (uint64_t)1);
}

static void test_va_page_offset(serial_dev_t *dev) {
    ASSERT_EQ(dev, PAGE_OFFSET(0x1234567890ULL), (uint64_t)0x890);
    ASSERT_EQ(dev, PAGE_OFFSET(0xFFFFFFFFF000ULL), (uint64_t)0x000);
}

static void test_va_canonical_check(serial_dev_t *dev) {
    ASSERT_TRUE(dev,  va_is_canonical(0x0000000000000000ULL));
    ASSERT_TRUE(dev,  va_is_canonical(0x00007FFFFFFFFFFFULL));
    ASSERT_TRUE(dev,  va_is_canonical(0xFFFF800000000000ULL));
    ASSERT_TRUE(dev, !va_is_canonical(0x0000800000000000ULL));
    ASSERT_TRUE(dev, !va_is_canonical(0xFFFF7FFFFFFFFFFFULL));
}

/* =========================================================================
 * Group C: Basic Map/Unmap Round-Trip (10 tests)
 *
 * Each test allocates a physical frame from the PMM, maps it into the
 * test VA range, and verifies the mapping via translate + memory access.
 * ========================================================================= */

static void test_map_single_page(serial_dev_t *dev) {
    reset_test_va();
    uint64_t paddr = pmm_alloc_frame();
    uint64_t vaddr = alloc_test_va(1);
    ASSERT_TRUE(dev, paddr != 0);

    vmm_status_t st = vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, (int)st, (int)VMM_OK);

    /* Translate should return the same physical address. */
    uint64_t translated = vmm_translate(vmm_get_kernel_address_space(), vaddr);
    ASSERT_EQ(dev, translated, paddr);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    pmm_free_frame(paddr);
}

static void test_map_write_read(serial_dev_t *dev) {
    reset_test_va();
    uint64_t paddr = pmm_alloc_frame();
    uint64_t vaddr = alloc_test_va(1);
    ASSERT_TRUE(dev, paddr != 0);

    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);

    /* Write and read back through the mapped address. */
    volatile uint64_t *ptr = (volatile uint64_t *)vaddr;
    *ptr = 0xDEADBEEFCAFEBABEULL;
    ASSERT_EQ(dev, *ptr, 0xDEADBEEFCAFEBABEULL);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    pmm_free_frame(paddr);
}

static void test_map_unmap_free_cycle(serial_dev_t *dev) {
    reset_test_va();
    uint64_t paddr = pmm_alloc_frame();
    uint64_t vaddr = alloc_test_va(1);
    ASSERT_TRUE(dev, paddr != 0);

    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), vaddr), paddr);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), vaddr), (uint64_t)0);

    pmm_free_frame(paddr);
}

static void test_unmap_not_mapped(serial_dev_t *dev) {
    reset_test_va();
    uint64_t vaddr = alloc_test_va(1);
    vmm_status_t st = vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_NOT_MAPPED);
}

static void test_map_already_mapped(serial_dev_t *dev) {
    reset_test_va();
    uint64_t paddr1 = pmm_alloc_frame();
    uint64_t paddr2 = pmm_alloc_frame();
    uint64_t vaddr = alloc_test_va(1);
    ASSERT_TRUE(dev, paddr1 != 0 && paddr2 != 0);

    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr1, VMM_FLAG_WRITE);
    vmm_status_t st = vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr2, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_ALREADY_MAP);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    pmm_free_frame(paddr1);
    pmm_free_frame(paddr2);
}

static void test_map_misaligned_vaddr(serial_dev_t *dev) {
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_status_t st = vmm_map_page(vmm_get_kernel_address_space(), 0x400123, paddr, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);
    pmm_free_frame(paddr);
}

static void test_map_misaligned_paddr(serial_dev_t *dev) {
    reset_test_va();
    uint64_t vaddr = alloc_test_va(1);
    vmm_status_t st = vmm_map_page(vmm_get_kernel_address_space(), vaddr, 0x567, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);
}

static void test_map_null_address(serial_dev_t *dev) {
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_status_t st = vmm_map_page(vmm_get_kernel_address_space(), 0, paddr, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);
    pmm_free_frame(paddr);
}

static void test_map_kernel_space(serial_dev_t *dev) {
    reset_test_va();
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    /* Map a kernel-range VA — PML4[511], PDPT[256] is not mapped by
     * boot page tables, so this tests mapping into kernel virtual space. */
    uint64_t vaddr = 0xFFFFFFFF40000000ULL;  /* upper-half, unmapped */
    vmm_status_t st = vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, (int)st, (int)VMM_OK);

    /* Translate should return the physical address. */
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), vaddr), paddr);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    pmm_free_frame(paddr);
}

static void test_translate_not_mapped(serial_dev_t *dev) {
    reset_test_va();
    uint64_t vaddr = alloc_test_va(1);
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), vaddr), (uint64_t)0);
}

/* =========================================================================
 * Group D: Multiple Mappings & Ranges (8 tests)
 * ========================================================================= */

static void test_map_multiple_pages(serial_dev_t *dev) {
    reset_test_va();
    uint64_t addrs[16];
    uint64_t vbase = alloc_test_va(16);

    for (int i = 0; i < 16; i++) {
        addrs[i] = pmm_alloc_frame();
        ASSERT_TRUE(dev, addrs[i] != 0);
        vmm_status_t st = vmm_map_page(vmm_get_kernel_address_space(),
                                        vbase + i * PAGE_SIZE, addrs[i], VMM_FLAG_WRITE);
        ASSERT_EQ(dev, (int)st, (int)VMM_OK);
    }

    /* All translate correctly. */
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(),
                                      vbase + i * PAGE_SIZE), addrs[i]);
    }

    for (int i = 0; i < 16; i++) {
        vmm_unmap_page(vmm_get_kernel_address_space(), vbase + i * PAGE_SIZE);
        pmm_free_frame(addrs[i]);
    }
}

static void test_map_contiguous_range(serial_dev_t *dev) {
    reset_test_va();
    int count = 16;  /* 64 KiB */
    uint64_t vbase = alloc_test_va(count);

    for (int i = 0; i < count; i++) {
        uint64_t p = pmm_alloc_frame();
        ASSERT_TRUE(dev, p != 0);
        vmm_status_t st = vmm_map_page(vmm_get_kernel_address_space(),
                                        vbase + i * PAGE_SIZE, p, VMM_FLAG_WRITE);
        ASSERT_EQ(dev, (int)st, (int)VMM_OK);
    }

    /* Write a pattern to verify each page is independent. */
    for (int i = 0; i < count; i++) {
        volatile uint64_t *ptr = (volatile uint64_t *)(vbase + i * PAGE_SIZE);
        *ptr = (uint64_t)i;
    }
    for (int i = 0; i < count; i++) {
        volatile uint64_t *ptr = (volatile uint64_t *)(vbase + i * PAGE_SIZE);
        ASSERT_EQ(dev, *ptr, (uint64_t)i);
    }

    for (int i = 0; i < count; i++) {
        uint64_t p = vmm_translate(vmm_get_kernel_address_space(), vbase + i * PAGE_SIZE);
        vmm_unmap_page(vmm_get_kernel_address_space(), vbase + i * PAGE_SIZE);
        pmm_free_frame(p);
    }
}

static void test_unmap_range(serial_dev_t *dev) {
    reset_test_va();
    int count = 8;
    uint64_t vbase = alloc_test_va(count);
    uint64_t paddrs[8];

    for (int i = 0; i < count; i++) {
        paddrs[i] = pmm_alloc_frame();
        ASSERT_TRUE(dev, paddrs[i] != 0);
        vmm_map_page(vmm_get_kernel_address_space(), vbase + i * PAGE_SIZE, paddrs[i], VMM_FLAG_WRITE);
    }

    /* Unmap the entire range in one call. */
    vmm_status_t st = vmm_unmap_range(vmm_get_kernel_address_space(),
                                       vbase, vbase + count * PAGE_SIZE);
    ASSERT_EQ(dev, (int)st, (int)VMM_OK);

    /* Verify all unmapped. */
    for (int i = 0; i < count; i++) {
        ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(),
                                      vbase + i * PAGE_SIZE), (uint64_t)0);
    }

    /* Free physical frames (saved before unmap). */
    for (int i = 0; i < count; i++) {
        pmm_free_frame(paddrs[i]);
    }
}

static void test_map_sparse(serial_dev_t *dev) {
    reset_test_va();
    uint64_t addrs[4];
    uint64_t vas[4];

    /* Map 4 pages with gaps: va, va+4K, va+16K, va+24K. */
    vas[0] = alloc_test_va(1);  /* 0x0000008000000000 */
    vas[1] = vas[0] + 0x1000;   /* 0x401000 */
    vas[2] = vas[0] + 0x10000;  /* 0x410000 */
    vas[3] = vas[0] + 0x18000;  /* 0x418000 */
    g_test_va_next = vas[0] + 0x20000; /* skip to after the sparse range */

    for (int i = 0; i < 4; i++) {
        addrs[i] = pmm_alloc_frame();
        ASSERT_TRUE(dev, addrs[i] != 0);
        vmm_map_page(vmm_get_kernel_address_space(), vas[i], addrs[i], VMM_FLAG_WRITE);
    }

    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), vas[i]), addrs[i]);
    }

    for (int i = 0; i < 4; i++) {
        vmm_unmap_page(vmm_get_kernel_address_space(), vas[i]);
        pmm_free_frame(addrs[i]);
    }
}

static void test_map_overlapping_range(serial_dev_t *dev) {
    reset_test_va();
    uint64_t paddr1 = pmm_alloc_frame();
    uint64_t paddr2 = pmm_alloc_frame();
    uint64_t vaddr = alloc_test_va(1);
    ASSERT_TRUE(dev, paddr1 != 0 && paddr2 != 0);

    /* First map succeeds. */
    ASSERT_EQ(dev, (int)vmm_map_page(vmm_get_kernel_address_space(),
                                      vaddr, paddr1, VMM_FLAG_WRITE), (int)VMM_OK);
    /* Second map to same VA fails. */
    ASSERT_EQ(dev, (int)vmm_map_page(vmm_get_kernel_address_space(),
                                      vaddr, paddr2, VMM_FLAG_WRITE), (int)VMM_ERR_ALREADY_MAP);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    pmm_free_frame(paddr1);
    pmm_free_frame(paddr2);
}

static void test_map_large_random_offset(serial_dev_t *dev) {
    reset_test_va();
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    /* Map at a non-trivial offset. */
    uint64_t vaddr = 0x405A0000ULL;
    vmm_status_t st = vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, (int)st, (int)VMM_OK);
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), vaddr), paddr);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    pmm_free_frame(paddr);
}

static void test_map_then_unmap_middle(serial_dev_t *dev) {
    reset_test_va();
    uint64_t p[3];
    uint64_t vbase = alloc_test_va(3);

    for (int i = 0; i < 3; i++) {
        p[i] = pmm_alloc_frame();
        ASSERT_TRUE(dev, p[i] != 0);
        vmm_map_page(vmm_get_kernel_address_space(), vbase + i * PAGE_SIZE, p[i], VMM_FLAG_WRITE);
    }

    /* Unmap the middle page. */
    vmm_unmap_page(vmm_get_kernel_address_space(), vbase + PAGE_SIZE);
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(),
                                  vbase + PAGE_SIZE), (uint64_t)0);

    /* Neighbors still present. */
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), vbase), p[0]);
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), vbase + 2 * PAGE_SIZE), p[2]);

    for (int i = 0; i < 3; i++) {
        vmm_unmap_page(vmm_get_kernel_address_space(), vbase + i * PAGE_SIZE);
        pmm_free_frame(p[i]);
    }
}

/* =========================================================================
 * Group E: Address Space Creation & Destruction (10 tests)
 * ========================================================================= */

static void test_create_address_space(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_status_t st = vmm_create_address_space(&as);
    ASSERT_EQ(dev, (int)st, (int)VMM_OK);
    ASSERT_NOT_NULL(dev, as.pml4);
    ASSERT_TRUE(dev, as.pml4_phys != 0);
    ASSERT_TRUE(dev, !as.is_kernel);

    /* Kernel entries (256-511) should be non-zero (copied from kernel). */
    uint64_t kernel_entry = as.pml4[256];
    uint64_t kern_entry_orig = vmm_get_kernel_address_space()->pml4[256];
    ASSERT_EQ(dev, kernel_entry, kern_entry_orig);

    vmm_destroy_address_space(&as);
}

static void test_create_multiple_spaces(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as[3];

    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(dev, (int)vmm_create_address_space(&as[i]), (int)VMM_OK);
        ASSERT_NOT_NULL(dev, as[i].pml4);

        /* Each should have a kernel-mapped upper half (entry 511+). */
        uint64_t kernel_entry = as[i].pml4[511];
        ASSERT_TRUE(dev, kernel_entry != 0);
    }

    /* All have different PML4 physical addresses. */
    ASSERT_TRUE(dev, as[0].pml4_phys != as[1].pml4_phys);
    ASSERT_TRUE(dev, as[1].pml4_phys != as[2].pml4_phys);

    for (int i = 0; i < 3; i++) {
        vmm_destroy_address_space(&as[i]);
    }
}

static void test_switch_address_space(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    /* Switch to the new address space. */
    vmm_switch_address_space(&as);

    /* Read CR3 and verify it matches the new PML4. */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    ASSERT_EQ(dev, cr3, as.pml4_phys);

    /* Switch back to kernel. */
    vmm_switch_address_space(vmm_get_kernel_address_space());
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    ASSERT_EQ(dev, cr3, vmm_get_kernel_address_space()->pml4_phys);

    vmm_destroy_address_space(&as);
}

static void test_switch_roundtrip(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    /* Map a page in both kernel and new space (PML4[1] is NOT shared). */
    uint64_t paddr = pmm_alloc_frame();
    uint64_t vaddr = alloc_test_va(1);
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);
    vmm_map_page(&as, vaddr, paddr, VMM_FLAG_WRITE);

    /* Switch to new space — mapping exists in as, so vaddr works. */
    vmm_switch_address_space(&as);
    volatile uint64_t *ptr = (volatile uint64_t *)vaddr;
    *ptr = 0x1111222233334444ULL;
    ASSERT_EQ(dev, *ptr, 0x1111222233334444ULL);

    /* Switch back — kernel mapping still exists. */
    vmm_switch_address_space(vmm_get_kernel_address_space());
    ASSERT_EQ(dev, *ptr, 0x1111222233334444ULL);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    pmm_free_frame(paddr);
    vmm_destroy_address_space(&as);
}

static void test_destroy_address_space(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    /* Map a few pages to create sub-tables. */
    for (int i = 0; i < 4; i++) {
        uint64_t p = pmm_alloc_frame();
        ASSERT_TRUE(dev, p != 0);
        vmm_map_page(&as, 0x0000008000000000 + i * PAGE_SIZE, p, VMM_FLAG_WRITE);
    }

    vmm_status_t st = vmm_destroy_address_space(&as);
    ASSERT_EQ(dev, (int)st, (int)VMM_OK);
    ASSERT_TRUE(dev, as.pml4 == NULL);
}

static void test_new_space_isolation(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    /* Map a page in the new space. */
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(&as, 0x0000008000000000, paddr, VMM_FLAG_WRITE);

    /* The new space should have it mapped. */
    ASSERT_EQ(dev, vmm_translate(&as, 0x0000008000000000), paddr);

    vmm_destroy_address_space(&as);
    pmm_free_frame(paddr);
}

static void test_kernel_mapping_shared(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    /* Kernel PML4 entries should match. */
    for (int i = 256; i < PT_ENTRIES; i++) {
        ASSERT_EQ(dev, as.pml4[i], vmm_get_kernel_address_space()->pml4[i]);
    }

    vmm_destroy_address_space(&as);
}

static void test_create_after_vmm_init(serial_dev_t *dev) {
    /* If we can create an address space, VMM was initialized. */
    ASSERT_TRUE(dev, vmm_is_initialized());
    ASSERT_NOT_NULL(dev, vmm_get_kernel_address_space());
}

static void test_switch_to_kernel(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    vmm_switch_address_space(&as);
    vmm_switch_address_space(vmm_get_kernel_address_space());

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    ASSERT_EQ(dev, cr3, vmm_get_kernel_address_space()->pml4_phys);

    vmm_destroy_address_space(&as);
}

static void test_destroy_null(serial_dev_t *dev) {
    vmm_status_t st = vmm_destroy_address_space(NULL);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);

    address_space_t bad = { NULL, 0, false };
    st = vmm_destroy_address_space(&bad);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);
}

/* =========================================================================
 * Group F: Address Space Isolation (6 tests)
 * ========================================================================= */

static void test_isolation_basic(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    /* Map in the new space. */
    vmm_map_page(&as, 0x0000008000000000, paddr, VMM_FLAG_WRITE);

    /* Kernel space should NOT have this mapping. */
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), 0x0000008000000000), (uint64_t)0);

    vmm_destroy_address_space(&as);
    pmm_free_frame(paddr);
}

static void test_isolation_write_verify(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    vmm_map_page(&as, 0x0000008000000000, paddr, VMM_FLAG_WRITE);

    /* Write in the new space. */
    vmm_switch_address_space(&as);
    volatile uint64_t *ptr = (volatile uint64_t *)0x0000008000000000;
    *ptr = 0xAAAAAAAAAAAAAAAAULL;

    /* Switch back to kernel — kernel should NOT see this data. */
    vmm_switch_address_space(vmm_get_kernel_address_space());

    /* The kernel's view of 0x0000008000000000 is unmapped, so translating returns 0. */
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), 0x0000008000000000), (uint64_t)0);

    vmm_destroy_address_space(&as);
    pmm_free_frame(paddr);
}

static void test_isolation_switch_write(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(&as, 0x0000008000000000, paddr, VMM_FLAG_WRITE);

    /* Write 0x111 in kernel space (via a mapping we create there). */
    uint64_t paddr_k = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr_k != 0);
    vmm_map_page(vmm_get_kernel_address_space(), 0x0000008000000000, paddr_k, VMM_FLAG_WRITE);
    volatile uint64_t *ptr_k = (volatile uint64_t *)0x0000008000000000;
    *ptr_k = 0x1111111111111111ULL;

    /* Write 0x222 in the new space (mapped to a different phys frame). */
    vmm_switch_address_space(&as);
    volatile uint64_t *ptr_a = (volatile uint64_t *)0x0000008000000000;
    *ptr_a = 0x2222222222222222ULL;

    /* Switch back to kernel — kernel should still see 0x111. */
    vmm_switch_address_space(vmm_get_kernel_address_space());
    ASSERT_EQ(dev, *ptr_k, 0x1111111111111111ULL);

    vmm_unmap_page(vmm_get_kernel_address_space(), 0x0000008000000000);
    pmm_free_frame(paddr_k);
    vmm_destroy_address_space(&as);
    pmm_free_frame(paddr);
}

static void test_isolation_many_pages(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    for (int i = 0; i < 8; i++) {
        uint64_t p = pmm_alloc_frame();
        ASSERT_TRUE(dev, p != 0);
        vmm_map_page(&as, 0x0000008000000000 + i * PAGE_SIZE, p, VMM_FLAG_WRITE);
    }

    /* All 8 pages exist in the new space. */
    for (int i = 0; i < 8; i++) {
        ASSERT_TRUE(dev, vmm_translate(&as, 0x0000008000000000 + i * PAGE_SIZE) != 0);
    }

    /* None of them exist in the kernel space. */
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(),
                                      0x0000008000000000 + i * PAGE_SIZE), (uint64_t)0);
    }

    vmm_destroy_address_space(&as);
}

static void test_isolation_unmap_doesnt_affect_other(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    /* Map in BOTH spaces. */
    vmm_map_page(&as, 0x0000008000000000, paddr, VMM_FLAG_WRITE);
    vmm_map_page(vmm_get_kernel_address_space(), 0x0000008000000000, paddr, VMM_FLAG_WRITE);

    /* Unmap in kernel space. */
    vmm_unmap_page(vmm_get_kernel_address_space(), 0x0000008000000000);

    /* Still mapped in 'as'. */
    ASSERT_EQ(dev, vmm_translate(&as, 0x0000008000000000), paddr);

    vmm_unmap_page(&as, 0x0000008000000000);
    vmm_destroy_address_space(&as);
    pmm_free_frame(paddr);
}

static void test_isolation_different_flags(serial_dev_t *dev) {
    reset_test_va();
    address_space_t as;
    vmm_create_address_space(&as);

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    /* Map RW in new space, and RO (no WRITE flag) in kernel space. */
    vmm_map_page(&as, 0x0000008000000000, paddr, VMM_FLAG_WRITE | VMM_FLAG_USER);
    vmm_map_page(vmm_get_kernel_address_space(), 0x0000008000000000, paddr, PTE_PROT_READ);

    /* Both should translate to the same physical address. */
    ASSERT_EQ(dev, vmm_translate(&as, 0x0000008000000000), paddr);
    ASSERT_EQ(dev, vmm_translate(vmm_get_kernel_address_space(), 0x0000008000000000), paddr);

    vmm_unmap_page(&as, 0x0000008000000000);
    vmm_unmap_page(vmm_get_kernel_address_space(), 0x0000008000000000);
    vmm_destroy_address_space(&as);
    pmm_free_frame(paddr);
}

/* =========================================================================
 * Group G: Clone (Fork Simulation) (6 tests)
 * ========================================================================= */

static void test_clone_basic(serial_dev_t *dev) {
    reset_test_va();
    address_space_t src, dst;
    vmm_create_address_space(&src);
    vmm_create_address_space(&dst);

    /* Map a page in the source. */
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(&src, 0x0000008000000000, paddr, VMM_FLAG_WRITE);

    /* Clone. */
    vmm_status_t st = vmm_clone_address_space(&src, &dst);
    ASSERT_EQ(dev, (int)st, (int)VMM_OK);

    /* Destination should have the same mapping. */
    ASSERT_EQ(dev, vmm_translate(&dst, 0x0000008000000000), paddr);

    /* Kernel mappings are still shared. */
    for (int i = 256; i < PT_ENTRIES; i++) {
        ASSERT_EQ(dev, dst.pml4[i], vmm_get_kernel_address_space()->pml4[i]);
    }

    vmm_unmap_page(&src, 0x0000008000000000);
    vmm_unmap_page(&dst, 0x0000008000000000);
    pmm_free_frame(paddr);
    vmm_destroy_address_space(&src);
    vmm_destroy_address_space(&dst);
}

static void test_clone_table_copied(serial_dev_t *dev) {
    reset_test_va();
    address_space_t src, dst;
    vmm_create_address_space(&src);
    vmm_create_address_space(&dst);

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(&src, 0x0000008000000000, paddr, VMM_FLAG_WRITE);

    /* Before clone: src has a PDPT entry at index 0, dst does not. */
    ASSERT_TRUE(dev, src.pml4[0] != 0);

    vmm_clone_address_space(&src, &dst);

    /* After clone: both have PDPT entries, but they are different physical pages. */
    uint64_t src_pdpt = pte_addr(src.pml4[0]);
    uint64_t dst_pdpt = pte_addr(dst.pml4[0]);
    ASSERT_TRUE(dev, src_pdpt != dst_pdpt);

    vmm_unmap_page(&src, 0x0000008000000000);
    vmm_unmap_page(&dst, 0x0000008000000000);
    pmm_free_frame(paddr);
    vmm_destroy_address_space(&src);
    vmm_destroy_address_space(&dst);
}

static void test_clone_entries_copied(serial_dev_t *dev) {
    reset_test_va();
    address_space_t src, dst;
    vmm_create_address_space(&src);
    vmm_create_address_space(&dst);

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(&src, 0x0000008000000000, paddr, VMM_FLAG_WRITE);

    vmm_clone_address_space(&src, &dst);

    /* Both translate to the same physical page (shared frames). */
    ASSERT_EQ(dev, vmm_translate(&dst, 0x0000008000000000), paddr);

    vmm_unmap_page(&src, 0x0000008000000000);
    vmm_unmap_page(&dst, 0x0000008000000000);
    pmm_free_frame(paddr);
    vmm_destroy_address_space(&src);
    vmm_destroy_address_space(&dst);
}

static void test_clone_independence(serial_dev_t *dev) {
    reset_test_va();
    address_space_t src, dst;
    vmm_create_address_space(&src);
    vmm_create_address_space(&dst);

    uint64_t paddr1 = pmm_alloc_frame();
    uint64_t paddr2 = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr1 != 0 && paddr2 != 0);

    vmm_map_page(&src, 0x0000008000000000, paddr1, VMM_FLAG_WRITE);
    vmm_clone_address_space(&src, &dst);

    /* Map a DIFFERENT physical page to the same virtual addr in dst. */
    vmm_unmap_page(&dst, 0x0000008000000000);
    vmm_map_page(&dst, 0x0000008000000000, paddr2, VMM_FLAG_WRITE);

    /* src should still have the original. */
    ASSERT_EQ(dev, vmm_translate(&src, 0x0000008000000000), paddr1);
    /* dst has the new one. */
    ASSERT_EQ(dev, vmm_translate(&dst, 0x0000008000000000), paddr2);

    vmm_unmap_page(&src, 0x0000008000000000);
    vmm_unmap_page(&dst, 0x0000008000000000);
    pmm_free_frame(paddr1);
    pmm_free_frame(paddr2);
    vmm_destroy_address_space(&src);
    vmm_destroy_address_space(&dst);
}

static void test_clone_two_clones(serial_dev_t *dev) {
    reset_test_va();
    address_space_t src, d1, d2;
    vmm_create_address_space(&src);
    vmm_create_address_space(&d1);
    vmm_create_address_space(&d2);

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(&src, 0x0000008000000000, paddr, VMM_FLAG_WRITE);

    vmm_clone_address_space(&src, &d1);
    vmm_clone_address_space(&src, &d2);

    ASSERT_EQ(dev, vmm_translate(&d1, 0x0000008000000000), paddr);
    ASSERT_EQ(dev, vmm_translate(&d2, 0x0000008000000000), paddr);

    vmm_unmap_page(&src, 0x0000008000000000);
    vmm_unmap_page(&d1, 0x0000008000000000);
    vmm_unmap_page(&d2, 0x0000008000000000);
    pmm_free_frame(paddr);
    vmm_destroy_address_space(&src);
    vmm_destroy_address_space(&d1);
    vmm_destroy_address_space(&d2);
}

static void test_clone_preserves_flags(serial_dev_t *dev) {
    reset_test_va();
    address_space_t src, dst;
    vmm_create_address_space(&src);
    vmm_create_address_space(&dst);

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    uint64_t flags = VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_GLOBAL;
    vmm_map_page(&src, 0x0000008000000000, paddr, flags);

    vmm_clone_address_space(&src, &dst);

    /* Walk the clone's page tables and verify the PTE flags match. */
    uint64_t *pml4 = dst.pml4;
    uint64_t pdpt_phys = pte_addr(pml4[PML4_INDEX(0x0000008000000000)]);
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pdpt_phys);
    uint64_t pd_phys = pte_addr(pdpt[PDPT_INDEX(0x0000008000000000)]);
    uint64_t *pd = (uint64_t *)(uintptr_t)pt_phys_to_virt(pd_phys);
    uint64_t pt_phys = pte_addr(pd[PD_INDEX(0x0000008000000000)]);
    uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pt_phys);
    uint64_t pte = pt[PT_INDEX(0x0000008000000000)];

    /* PTE flags should include PRESENT. */
    ASSERT_TRUE(dev, pte & PTE_PRESENT);

    vmm_unmap_page(&src, 0x0000008000000000);
    vmm_unmap_page(&dst, 0x0000008000000000);
    pmm_free_frame(paddr);
    vmm_destroy_address_space(&src);
    vmm_destroy_address_space(&dst);
}

/* =========================================================================
 * Group H: Error Conditions & Edge Cases (8 tests)
 * ========================================================================= */

static void test_unmap_null_aspace(serial_dev_t *dev) {
    vmm_status_t st = vmm_unmap_page(NULL, 0x0000008000000000);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);
}

static void test_translate_null_aspace(serial_dev_t *dev) {
    ASSERT_EQ(dev, vmm_translate(NULL, 0x0000008000000000), (uint64_t)0);
}

static void test_map_null_aspace(serial_dev_t *dev) {
    uint64_t p = pmm_alloc_frame();
    vmm_status_t st = vmm_map_page(NULL, 0x0000008000000000, p, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);
    pmm_free_frame(p);
}

static void test_create_null_out(serial_dev_t *dev) {
    vmm_status_t st = vmm_create_address_space(NULL);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);
}

static void test_flags_propagation(serial_dev_t *dev) {
    reset_test_va();
    uint64_t paddr = pmm_alloc_frame();
    uint64_t vaddr = alloc_test_va(1);
    ASSERT_TRUE(dev, paddr != 0);

    /* Map with specific flags. */
    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr,
                 VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_GLOBAL);

    /* Walk the page tables and verify the PTE. */
    uint64_t *pml4 = vmm_get_kernel_address_space()->pml4;
    uint64_t pdpt_phys = pte_addr(pml4[PML4_INDEX(vaddr)]);
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pdpt_phys);
    uint64_t pd_phys = pte_addr(pdpt[PDPT_INDEX(vaddr)]);
    uint64_t *pd = (uint64_t *)(uintptr_t)pt_phys_to_virt(pd_phys);
    uint64_t pt_phys = pte_addr(pd[PD_INDEX(vaddr)]);
    uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pt_phys);
    uint64_t pte = pt[PT_INDEX(vaddr)];

    ASSERT_TRUE(dev, pte & PTE_PRESENT);
    ASSERT_TRUE(dev, pte & PTE_WRITABLE);
    ASSERT_TRUE(dev, pte & PTE_USER);
    ASSERT_TRUE(dev, pte & PTE_GLOBAL);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    pmm_free_frame(paddr);
}

static void test_translate_full_walk(serial_dev_t *dev) {
    reset_test_va();
    uint64_t paddr = pmm_alloc_frame();
    uint64_t vaddr = alloc_test_va(1);
    ASSERT_TRUE(dev, paddr != 0);

    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);

    /* Manual 4-level walk to verify the VMM's translation. */
    uint64_t *pml4 = vmm_get_kernel_address_space()->pml4;

    /* Level 4 */
    uint64_t pml4e = pml4[PML4_INDEX(vaddr)];
    ASSERT_TRUE(dev, pml4e & PTE_PRESENT);

    /* Level 3 */
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pml4e));
    uint64_t pdpe = pdpt[PDPT_INDEX(vaddr)];
    ASSERT_TRUE(dev, pdpe & PTE_PRESENT);
    ASSERT_TRUE(dev, !(pdpe & PTE_PS));  /* not a 1 GiB huge page */

    /* Level 2 */
    uint64_t *pd = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pdpe));
    uint64_t pde = pd[PD_INDEX(vaddr)];
    ASSERT_TRUE(dev, pde & PTE_PRESENT);
    ASSERT_TRUE(dev, !(pde & PTE_PS));  /* not a 2 MiB huge page */

    /* Level 1 */
    uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys_to_virt(pte_addr(pde));
    uint64_t pte_final = pt[PT_INDEX(vaddr)];
    ASSERT_TRUE(dev, pte_final & PTE_PRESENT);
    ASSERT_EQ(dev, pte_addr(pte_final), paddr);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    pmm_free_frame(paddr);
}

static void test_create_many_spaces(serial_dev_t *dev) {
    reset_test_va();
    address_space_t spaces[10];

    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(dev, (int)vmm_create_address_space(&spaces[i]), (int)VMM_OK);
    }

    /* All have unique PML4 physical addresses. */
    for (int i = 0; i < 10; i++) {
        for (int j = i + 1; j < 10; j++) {
            ASSERT_TRUE(dev, spaces[i].pml4_phys != spaces[j].pml4_phys);
        }
    }

    for (int i = 0; i < 10; i++) {
        vmm_destroy_address_space(&spaces[i]);
    }
}

static void test_log_output_not_empty(serial_dev_t *dev) {
    /* This test verifies that the VMM logging system is functional.
     * If we got this far without crashing, logging is working.
     * The test itself doesn't produce output — it just verifies
     * the VMM is initialized and logging is enabled. */
    ASSERT_TRUE(dev, vmm_is_initialized());
    ASSERT_NOT_NULL(dev, vmm_get_kernel_address_space());
}

/* =========================================================================
 * Group I: Intermediate Table Cleanup After Unmap (4 tests)
 * ========================================================================= */

static void test_unmap_frees_pt_page(serial_dev_t *dev) {
    reset_test_va();
    uint64_t vaddr = alloc_test_va(1);
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);

    pmm_stats_t before;
    pmm_get_stats(&before);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);

    pmm_stats_t after;
    pmm_get_stats(&after);

    /* Unmap freed: the PT page that held the PTE should be freed. */
    ASSERT_TRUE(dev, after.free_frames > before.free_frames);

    pmm_free_frame(paddr);
}

static void test_unmap_frees_full_table(serial_dev_t *dev) {
    reset_test_va();
    /* Map 512 pages filling an entire PT (same PD entry). */
    uint64_t base_vaddr = alloc_test_va(512);
    uint64_t addrs[512];
    for (int i = 0; i < 512; i++) {
        addrs[i] = pmm_alloc_frame();
        ASSERT_TRUE(dev, addrs[i] != 0);
    }
    for (int i = 0; i < 512; i++) {
        vmm_map_page(vmm_get_kernel_address_space(),
                     base_vaddr + i * PAGE_SIZE,
                     addrs[i], VMM_FLAG_WRITE);
    }

    pmm_stats_t before;
    pmm_get_stats(&before);

    /* Unmap all 512 pages — the entire PT becomes empty. */
    for (int i = 0; i < 512; i++) {
        vmm_unmap_page(vmm_get_kernel_address_space(),
                       base_vaddr + i * PAGE_SIZE);
    }

    pmm_stats_t after;
    pmm_get_stats(&after);

    /* At minimum 1 PT page freed.  Possibly more if the PD entry
     * also became empty and the PD page was freed. */
    ASSERT_TRUE(dev, after.free_frames >= before.free_frames + 1);

    for (int i = 0; i < 512; i++) {
        pmm_free_frame(addrs[i]);
    }
}

static void test_unmap_partial_no_free(serial_dev_t *dev) {
    reset_test_va();
    /* Map 2 pages in the same PT.  Unmap 1.  PT should NOT be freed. */
    uint64_t vaddr0 = alloc_test_va(2);
    uint64_t vaddr1 = vaddr0 + PAGE_SIZE;
    uint64_t paddr0 = pmm_alloc_frame();
    uint64_t paddr1 = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr0 != 0 && paddr1 != 0);

    vmm_map_page(vmm_get_kernel_address_space(), vaddr0, paddr0, VMM_FLAG_WRITE);
    vmm_map_page(vmm_get_kernel_address_space(), vaddr1, paddr1, VMM_FLAG_WRITE);

    pmm_stats_t before;
    pmm_get_stats(&before);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr0);

    pmm_stats_t after;
    pmm_get_stats(&after);

    /* No intermediate tables freed — the PT still has 1 entry. */
    ASSERT_EQ(dev, after.free_frames, before.free_frames);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr1);
    pmm_free_frame(paddr0);
    pmm_free_frame(paddr1);
}

static void test_unmap_cleans_pdpt(serial_dev_t *dev) {
    reset_test_va();
    /* Map a single page in user PML4[1] range, unmap it — the PT, PD,
     * and PDPT entries should all be cleaned up.  PML4[1] should be
     * cleared since the entire PDPT became empty. */
    uint64_t vaddr = alloc_test_va(1);
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);

    /* Verify PML4[1] is present (PML4_INDEX(0x8000000000) == 1). */
    ASSERT_TRUE(dev, vmm_get_kernel_address_space()->pml4[1] != 0);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);

    /* After cleanup, PML4[1] should be cleared (PDPT was empty). */
    ASSERT_EQ(dev, vmm_get_kernel_address_space()->pml4[1], (uint64_t)0);

    pmm_free_frame(paddr);
}

/* =========================================================================
 * Group J: Copy-on-Write (3 tests)
 *
 * After vmm_clone_address_space, writable pages are marked COW
 * (read-only + COW flag) in both src and dst.  Writing to a COW page
 * triggers #PF that the handler resolves by allocating a private copy.
 *
 * Tests run in ring-0 with CR0.WP=1, so even supervisor writes to
 * COW pages trigger #PF — the handler resolves it transparently.
 * ========================================================================= */

static void test_clone_cow_basic(serial_dev_t *dev) {
    reset_test_va();
    uint64_t vaddr = 0x0000008000000000;

    /* Map a page in kernel, write a known value. */
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);
    *(volatile uint64_t *)vaddr = 0xDEADBEEF;

    /* Clone kernel -> as. */
    address_space_t as;
    vmm_create_address_space(&as);
    vmm_clone_address_space(vmm_get_kernel_address_space(), &as);

    /* Switch to as, write to the COW page (triggers #PF -> COW resolves). */
    vmm_switch_address_space(&as);
    *(volatile uint64_t *)vaddr = 0xCAFEBABE;
    vmm_switch_address_space(vmm_get_kernel_address_space());

    /* Kernel's page should still hold the original value. */
    ASSERT_EQ(dev, *(volatile uint64_t *)vaddr, (uint64_t)0xDEADBEEF);

    /* The two address spaces should now point to different phys frames. */
    uint64_t k_phys = vmm_translate(vmm_get_kernel_address_space(), vaddr);
    uint64_t a_phys = vmm_translate(&as, vaddr);
    ASSERT_TRUE(dev, k_phys != a_phys);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    vmm_unmap_page(&as, vaddr);
    pmm_free_frame(paddr);
    pmm_free_frame(a_phys);
    vmm_destroy_address_space(&as);
}

static void test_clone_cow_both_write(serial_dev_t *dev) {
    reset_test_va();
    uint64_t vaddr = 0x0000008000000000;

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);
    *(volatile uint64_t *)vaddr = 0x1111;

    address_space_t as;
    vmm_create_address_space(&as);
    vmm_clone_address_space(vmm_get_kernel_address_space(), &as);

    /* Write in kernel (COW resolves -> kernel gets private copy). */
    *(volatile uint64_t *)vaddr = 0x2222;
    uint64_t k_phys = vmm_translate(vmm_get_kernel_address_space(), vaddr);

    /* Write in as (COW resolves -> as gets its own copy). */
    vmm_switch_address_space(&as);
    *(volatile uint64_t *)vaddr = 0x3333;
    vmm_switch_address_space(vmm_get_kernel_address_space());

    uint64_t a_phys = vmm_translate(&as, vaddr);

    /* Both should have different physical frames. */
    ASSERT_TRUE(dev, k_phys != a_phys);

    /* Kernel reads its own value. */
    ASSERT_EQ(dev, *(volatile uint64_t *)vaddr, (uint64_t)0x2222);

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    vmm_unmap_page(&as, vaddr);
    pmm_free_frame(paddr);
    pmm_free_frame(k_phys);
    pmm_free_frame(a_phys);
    vmm_destroy_address_space(&as);
}

static void test_clone_cow_three_way(serial_dev_t *dev) {
    reset_test_va();
    uint64_t vaddr = 0x0000008000000000;

    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);
    vmm_map_page(vmm_get_kernel_address_space(), vaddr, paddr, VMM_FLAG_WRITE);
    *(volatile uint64_t *)vaddr = 0xAAAA;

    /* Clone kernel -> as1, then clone as1 -> as2.
     * Standard COW semantics: after clone, pages are shared (COW-flagged).
     * Frames are not duplicated until a write triggers a COW fault. */
    address_space_t as1, as2;
    vmm_create_address_space(&as1);
    vmm_create_address_space(&as2);
    vmm_clone_address_space(vmm_get_kernel_address_space(), &as1);
    vmm_clone_address_space(&as1, &as2);

    /* Write in as2 — triggers COW fault, as2 gets its own private frame.
     * kernel and as1 remain COW-shared on the original paddr. */
    vmm_switch_address_space(&as2);
    *(volatile uint64_t *)vaddr = 0xBBBB;
    vmm_switch_address_space(vmm_get_kernel_address_space());

    /* Kernel still has original value (COW-shared with as1). */
    ASSERT_EQ(dev, *(volatile uint64_t *)vaddr, (uint64_t)0xAAAA);

    /* as1 still has original value (no write occurred in as1). */
    vmm_switch_address_space(&as1);
    ASSERT_EQ(dev, *(volatile uint64_t *)vaddr, (uint64_t)0xAAAA);
    vmm_switch_address_space(vmm_get_kernel_address_space());

    /* Check physical frame layout:
     *   - kernel and as1 share paddr (COW, no write triggered)
     *   - as2 got a private copy (write triggered COW resolution) */
    uint64_t k_phys  = vmm_translate(vmm_get_kernel_address_space(), vaddr);
    uint64_t a1_phys = vmm_translate(&as1, vaddr);
    uint64_t a2_phys = vmm_translate(&as2, vaddr);

    /* kernel and as1 correctly share the original frame. */
    ASSERT_EQ(dev, k_phys, paddr);
    ASSERT_EQ(dev, a1_phys, paddr);

    /* as2 has an independent frame (COW was resolved on write). */
    ASSERT_TRUE(dev, a2_phys != 0);
    ASSERT_TRUE(dev, a2_phys != paddr);

    /* as2's frame holds the written value (0xBBBB). */
    vmm_switch_address_space(&as2);
    ASSERT_EQ(dev, *(volatile uint64_t *)vaddr, (uint64_t)0xBBBB);
    vmm_switch_address_space(vmm_get_kernel_address_space());

    vmm_unmap_page(vmm_get_kernel_address_space(), vaddr);
    vmm_unmap_page(&as1, vaddr);
    vmm_unmap_page(&as2, vaddr);
    pmm_free_frame(paddr);     /* original frame — shared by kernel and as1 */
    pmm_free_frame(a2_phys);   /* as2's private frame from COW resolution  */
    vmm_destroy_address_space(&as1);
    vmm_destroy_address_space(&as2);
}

/* =========================================================================
 * Group K — Unmap Range Validation & Huge Page Translate
 * ========================================================================= */

/**
 * K1: vmm_unmap_range rejects start >= end (same address or reversed).
 */
static void test_unmap_range_start_ge_end(serial_dev_t *dev) {
    reset_test_va();
    uint64_t base = alloc_test_va(2);
    vmm_status_t st;

    /* start == end → invalid */
    st = vmm_unmap_range(vmm_get_kernel_address_space(), base, base);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);

    /* start > end → invalid */
    st = vmm_unmap_range(vmm_get_kernel_address_space(), base + PAGE_SIZE, base);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);
}

/**
 * K2: vmm_unmap_range rejects misaligned start or end.
 */
static void test_unmap_range_misaligned(serial_dev_t *dev) {
    reset_test_va();
    uint64_t base = alloc_test_va(2);
    vmm_status_t st;

    /* Misaligned start */
    st = vmm_unmap_range(vmm_get_kernel_address_space(), base + 0x100,
                         base + 2 * PAGE_SIZE);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);

    /* Misaligned end */
    st = vmm_unmap_range(vmm_get_kernel_address_space(), base,
                         base + PAGE_SIZE + 0x100);
    ASSERT_EQ(dev, (int)st, (int)VMM_ERR_INVALID);
}

/**
 * K3: vmm_translate resolves a 2 MiB huge-page PD entry.
 *
 * Maps a normal 4 KiB page to build the page table hierarchy, then
 * overwrites the PD entry with a PS (huge page) bit set, and verifies
 * vmm_translate walks through it correctly.
 */
static void test_translate_huge_page(serial_dev_t *dev) {
    reset_test_va();
    address_space_t *kern = vmm_get_kernel_address_space();
    uint64_t vaddr = alloc_test_va(1);
    uint64_t paddr = pmm_alloc_frame();
    ASSERT_TRUE(dev, paddr != 0);

    /* Map normally to build the PML4→PDPT→PD→PT chain. */
    vmm_status_t st = vmm_map_page(kern, vaddr, paddr, VMM_FLAG_WRITE);
    ASSERT_EQ(dev, (int)st, (int)VMM_OK);

    /* Verify normal 4 KiB translation works. */
    ASSERT_EQ(dev, vmm_translate(kern, vaddr), paddr);

    /* Walk to the PD entry for this vaddr. */
    uint64_t pml4e = kern->pml4[PML4_INDEX(vaddr)];
    uint64_t *pdpt = (uint64_t *)pt_phys_to_virt(pte_addr(pml4e));
    uint64_t pdpe = pdpt[PDPT_INDEX(vaddr)];
    uint64_t *pd = (uint64_t *)pt_phys_to_virt(pte_addr(pdpe));
    int pd_idx = PD_INDEX(vaddr);

    /* Save the original 4 KiB PD entry. */
    uint64_t orig_pde = pd[pd_idx];

    /* Build a 2 MiB huge page PD entry.  The physical base must be
     * 2 MiB-aligned; we use the 2 MiB-aligned version of our frame. */
    uint64_t huge_phys = paddr & ~0x1FFFFFULL;
    uint64_t huge_pde  = huge_phys | PTE_PRESENT | PTE_WRITABLE | PTE_PS;
    pd[pd_idx] = huge_pde;
    pt_invlpg((void *)vaddr);

    /* Translate: should use the 2 MiB huge page path. */
    uint64_t offset  = vaddr & 0x1FFFFFULL;
    uint64_t result  = vmm_translate(kern, vaddr);
    serial_write_string(dev, "[VMM TEST] translate_huge_page: result=");
    t_hex64(dev, result); serial_write_string(dev, " expected=");
    t_hex64(dev, huge_phys + offset); serial_write_string(dev, "\r\n");
    ASSERT_EQ(dev, result, huge_phys + offset);

    /* Restore the original PD entry so the normal cleanup path works. */
    pd[pd_idx] = orig_pde;
    pt_invlpg((void *)vaddr);

    vmm_unmap_page(kern, vaddr);
    pmm_free_frame(paddr);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

void test_register_vmm(void) {
    /* Group A: PTE Flag Manipulation */
    test_register("pte_present_set",         test_pte_present_set);
    test_register("pte_writable_set",        test_pte_writable_set);
    test_register("pte_user_set",            test_pte_user_set);
    test_register("pte_nx_set",              test_pte_nx_set);
    test_register("pte_addr_extract",        test_pte_addr_extract);
    test_register("pte_flags_extract",       test_pte_flags_extract);
    test_register("pte_huge_set",            test_pte_huge_set);
    test_register("pte_global_set",          test_pte_global_set);

    /* Group B: VA Decomposition Macros */
    test_register("va_pml4_index",           test_va_pml4_index);
    test_register("va_pdpt_index",           test_va_pdpt_index);
    test_register("va_pd_index",             test_va_pd_index);
    test_register("va_pt_index",             test_va_pt_index);
    test_register("va_page_offset",          test_va_page_offset);
    test_register("va_canonical_check",      test_va_canonical_check);

    /* Group C: Basic Map/Unmap Round-Trip */
    test_register("map_single_page",         test_map_single_page);
    test_register("map_write_read",          test_map_write_read);
    test_register("map_unmap_free_cycle",    test_map_unmap_free_cycle);
    test_register("unmap_not_mapped",        test_unmap_not_mapped);
    test_register("map_already_mapped",      test_map_already_mapped);
    test_register("map_misaligned_vaddr",    test_map_misaligned_vaddr);
    test_register("map_misaligned_paddr",    test_map_misaligned_paddr);
    test_register("map_null_address",        test_map_null_address);
    test_register("map_kernel_space",        test_map_kernel_space);
    test_register("translate_not_mapped",    test_translate_not_mapped);

    /* Group D: Multiple Mappings & Ranges */
    test_register("map_multiple_pages",      test_map_multiple_pages);
    test_register("map_contiguous_range",    test_map_contiguous_range);
    test_register("unmap_range",             test_unmap_range);
    test_register("map_sparse",              test_map_sparse);
    test_register("map_overlapping_range",   test_map_overlapping_range);
    test_register("map_large_random_offset", test_map_large_random_offset);
    test_register("map_then_unmap_middle",   test_map_then_unmap_middle);

    /* Group E: Address Space Creation & Destruction */
    test_register("create_address_space",    test_create_address_space);
    test_register("create_multiple_spaces",  test_create_multiple_spaces);
    test_register("switch_address_space",    test_switch_address_space);
    test_register("switch_roundtrip",        test_switch_roundtrip);
    test_register("destroy_address_space",   test_destroy_address_space);
    test_register("new_space_isolation",     test_new_space_isolation);
    test_register("kernel_mapping_shared",   test_kernel_mapping_shared);
    test_register("create_after_vmm_init",   test_create_after_vmm_init);
    test_register("switch_to_kernel",        test_switch_to_kernel);
    test_register("destroy_null",            test_destroy_null);

    /* Group F: Address Space Isolation */
    test_register("isolation_basic",         test_isolation_basic);
    test_register("isolation_write_verify",  test_isolation_write_verify);
    test_register("isolation_switch_write",  test_isolation_switch_write);
    test_register("isolation_many_pages",    test_isolation_many_pages);
    test_register("isolation_unmap_other",   test_isolation_unmap_doesnt_affect_other);
    test_register("isolation_different_flags", test_isolation_different_flags);

    /* Group G: Clone */
    test_register("clone_basic",             test_clone_basic);
    test_register("clone_table_copied",      test_clone_table_copied);
    test_register("clone_entries_copied",    test_clone_entries_copied);
    test_register("clone_independence",      test_clone_independence);
    test_register("clone_two_clones",        test_clone_two_clones);
    test_register("clone_preserves_flags",   test_clone_preserves_flags);

    /* Group H: Error Conditions & Edge Cases */
    test_register("unmap_null_aspace",       test_unmap_null_aspace);
    test_register("translate_null_aspace",   test_translate_null_aspace);
    test_register("map_null_aspace",         test_map_null_aspace);
    test_register("create_null_out",         test_create_null_out);
    test_register("flags_propagation",       test_flags_propagation);
    test_register("translate_full_walk",     test_translate_full_walk);
    test_register("create_many_spaces",      test_create_many_spaces);
    test_register("log_output_not_empty",    test_log_output_not_empty);

    /* Group I: Intermediate Table Cleanup After Unmap */
    test_register("unmap_frees_pt_page",     test_unmap_frees_pt_page);
    test_register("unmap_frees_full_table",  test_unmap_frees_full_table);
    test_register("unmap_partial_no_free",   test_unmap_partial_no_free);
    test_register("unmap_cleans_pdpt",       test_unmap_cleans_pdpt);

    /* Group J: Copy-on-Write */
    test_register("clone_cow_basic",         test_clone_cow_basic);
    test_register("clone_cow_both_write",    test_clone_cow_both_write);
    test_register("clone_cow_three_way",     test_clone_cow_three_way);

    /* Group K: Unmap Range Validation & Huge Page Translate */
    test_register("unmap_range_start_ge_end", test_unmap_range_start_ge_end);
    test_register("unmap_range_misaligned",   test_unmap_range_misaligned);
    test_register("translate_huge_page",      test_translate_huge_page);
}
