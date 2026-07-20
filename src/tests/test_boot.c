#include "test.h"
#include <stdint.h>

/* Globals set by kernel_main before tests run */
extern uint32_t boot_magic;
extern uint32_t boot_info_ptr;

/* ---- page table walking helpers ---- */

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

#define PML4_INDEX(addr)  (((uint64_t)(addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr)  (((uint64_t)(addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)    (((uint64_t)(addr) >> 21) & 0x1FF)

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_HUGE     (1ULL << 7)

/* ---- tests ---- */

static void test_multiboot_magic(serial_dev_t *dev) {
    ASSERT_EQ(dev, boot_magic, 0x36d76289);
}

static void test_multiboot_info(serial_dev_t *dev) {
    ASSERT_TRUE(dev, boot_info_ptr != 0);
}

static void test_cr3_nonzero(serial_dev_t *dev) {
    uint64_t cr3 = read_cr3();
    ASSERT_TRUE(dev, cr3 != 0);
    ASSERT_TRUE(dev, (cr3 & 0xFFF) == 0); /* page-aligned */
}

static void test_pml4_present(serial_dev_t *dev) {
    uint64_t cr3 = read_cr3();
    uint64_t *pml4 = (uint64_t *)(cr3 & ~0xFFFULL);
    uint64_t entry = pml4[0];
    ASSERT_TRUE(dev, (entry & PAGE_PRESENT) != 0);
    ASSERT_TRUE(dev, (entry & PAGE_WRITABLE) != 0);
}

static void test_pdpt_present(serial_dev_t *dev) {
    uint64_t cr3 = read_cr3();
    uint64_t *pml4 = (uint64_t *)(cr3 & ~0xFFFULL);
    uint64_t *pdpt = (uint64_t *)(pml4[0] & ~0xFFFULL);
    ASSERT_TRUE(dev, (pdpt[0] & PAGE_PRESENT) != 0);
    ASSERT_TRUE(dev, (pdpt[0] & PAGE_WRITABLE) != 0);
}

static void test_pd_huge_present(serial_dev_t *dev) {
    uint64_t cr3 = read_cr3();
    uint64_t *pml4 = (uint64_t *)(cr3 & ~0xFFFULL);
    uint64_t *pdpt = (uint64_t *)(pml4[0] & ~0xFFFULL);
    uint64_t *pd   = (uint64_t *)(pdpt[0] & ~0xFFFULL);

    /* All 512 entries should be present + writable + huge (2MiB pages) */
    for (int i = 0; i < 512; i++) {
        uint64_t e = pd[i];
        if ((e & PAGE_PRESENT) == 0) {
            serial_write_string(dev, "FAIL PD entry not present: ");
            _print_int(dev, i);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
        if ((e & PAGE_WRITABLE) == 0) {
            serial_write_string(dev, "FAIL PD entry not writable: ");
            _print_int(dev, i);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
        if ((e & PAGE_HUGE) == 0) {
            serial_write_string(dev, "FAIL PD entry not huge: ");
            _print_int(dev, i);
            serial_write_string(dev, "\r\n");
            test_fail_flag = 1;
            return;
        }
    }
}

static void test_identity_map_readwrite(serial_dev_t *dev) {
    /* Address 2MiB is safely within identity-mapped first 1GiB
     * and well above the kernel. Use a pattern that isn't zero. */
    volatile uint32_t *ptr = (volatile uint32_t *)0x200000;
    uint32_t old = *ptr;    /* save */
    *ptr = 0xDEADBEEF;
    ASSERT_EQ(dev, *ptr, (uint32_t)0xDEADBEEF);
    *ptr = old;             /* restore */
}

void test_register_boot(void) {
    test_register("multiboot_magic",     test_multiboot_magic);
    test_register("multiboot_info_ptr",  test_multiboot_info);
    test_register("cr3_nonzero_aligned", test_cr3_nonzero);
    test_register("pml4_entry_valid",    test_pml4_present);
    test_register("pdpt_entry_valid",    test_pdpt_present);
    test_register("pd_entries_huge",     test_pd_huge_present);
    test_register("identity_map_rw",     test_identity_map_readwrite);
}
