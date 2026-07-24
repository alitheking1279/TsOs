/**
 * @file test_elf.c
 * @brief Tests for ELF64 loader — validation, loading, demand paging.
 *
 * Embeds a minimal valid ELF64 binary as test data to exercise the
 * ELF validation and loading code paths.
 */

#include "test.h"
#include "../kernel/elf.h"
#include "../kernel/vmm.h"
#include "../kernel/pmm.h"
#include "../kernel/page_table.h"
#include "../drivers/serial.h"
#include <string.h>

/* =========================================================================
 * Embedded Minimal ELF64 Binary
 *
 * A tiny valid ELF64 executable: ELF header + 1 PT_LOAD segment
 * containing a single RET instruction (0xC3).
 *
 * Layout:
 *   Offset 0x00: ELF header (64 bytes)
 *   Offset 0x40: Program header (56 bytes)
 *   Offset 0x78: Code (1 byte: 0xC3 = RET)
 *
 * Target addresses:
 *   e_entry = 0x400000
 *   p_vaddr = 0x400000
 *   p_offset = 0x00
 *   p_filesz = 1
 *   p_memsz  = 1
 *   p_flags  = PF_R | PF_X (5)
 * ========================================================================= */

static const uint8_t mini_elf[] = {
    /* === ELF64 Header (64 bytes at offset 0x00) === */
    0x7F, 'E', 'L', 'F',     /* e_ident[0..3]:  magic */
    2,                         /* e_ident[4]:     ELFCLASS64 */
    1,                         /* e_ident[5]:     ELFDATA2LSB (little-endian) */
    1,                         /* e_ident[6]:     EV_CURRENT */
    ELFOSABI_NONE,             /* e_ident[7]:     ELFOSABI_NONE */
    0, 0, 0, 0, 0, 0, 0, 0,  /* e_ident[8..15]: padding */

    0x02, 0x00,                /* e_type:     ET_EXEC (2) */
    0x3E, 0x00,                /* e_machine:  EM_X86_64 (0x3E) */
    0x01, 0x00, 0x00, 0x00,   /* e_version:  1 */

    /* e_entry: 0x0000000000400000 (little-endian) */
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* e_phoff: 0x40 (64 bytes) */
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* e_shoff: 0 (no section headers) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* e_flags: 0 */
    0x00, 0x00, 0x00, 0x00,

    /* e_ehsize: 64 */
    0x40, 0x00,

    /* e_phentsize: 56 */
    0x38, 0x00,

    /* e_phnum: 1 */
    0x01, 0x00,

    /* e_shentsize: 0 */
    0x00, 0x00,

    /* e_shnum: 0 */
    0x00, 0x00,

    /* e_shstrndx: 0 */
    0x00, 0x00,

    /* === Program Header #1 (56 bytes at offset 0x40) === */
    /* p_type: PT_LOAD (1) */
    0x01, 0x00, 0x00, 0x00,

    /* p_flags: PF_R | PF_X (5) */
    0x05, 0x00, 0x00, 0x00,

    /* p_offset: 0x78 (points to the RET instruction at offset 0x78) */
    0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* p_vaddr: 0x400000 */
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* p_paddr: 0x400000 (ignored for user loading) */
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* p_filesz: 1 */
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* p_memsz: 1 */
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* p_align: 0x1000 */
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* === Code (1 byte at offset 0x78) === */
    0xC3,  /* RET */
};

/* =========================================================================
 * Test: Validation of valid ELF
 * ========================================================================= */

static void test_elf_valid_header(serial_dev_t *dev) {
    elf64_header_t hdr;
    elf_status_t st = elf_validate(mini_elf, sizeof(mini_elf), &hdr);
    ASSERT_EQ(dev, st, ELF_OK);

    /* Verify header fields. */
    ASSERT_EQ(dev, hdr.e_type, ET_EXEC);
    ASSERT_EQ(dev, hdr.e_machine, EM_X86_64);
    ASSERT_EQ(dev, hdr.e_entry, 0x400000ULL);
    ASSERT_EQ(dev, hdr.e_phoff, 64);
    ASSERT_EQ(dev, hdr.e_phnum, 1);
    ASSERT_EQ(dev, hdr.e_phentsize, 56);
}

/* =========================================================================
 * Test: Rejection of bad magic
 * ========================================================================= */

static void test_elf_bad_magic(serial_dev_t *dev) {
    uint8_t bad[64];
    memcpy(bad, mini_elf, sizeof(bad));
    bad[0] = 0x00;  /* Corrupt magic */

    elf64_header_t hdr;
    elf_status_t st = elf_validate(bad, sizeof(bad), &hdr);
    ASSERT_EQ(dev, st, ELF_ERR_INVALID);
}

/* =========================================================================
 * Test: Rejection of too-small buffer
 * ========================================================================= */

static void test_elf_too_small(serial_dev_t *dev) {
    uint8_t tiny[4] = { 0x7F, 'E', 'L', 'F' };
    elf64_header_t hdr;
    elf_status_t st = elf_validate(tiny, sizeof(tiny), &hdr);
    ASSERT_EQ(dev, st, ELF_ERR_INVALID);
}

/* =========================================================================
 * Test: Rejection of non-executable (ET_REL)
 * ========================================================================= */

static void test_elf_not_executable(serial_dev_t *dev) {
    uint8_t rel[64];
    memcpy(rel, mini_elf, sizeof(rel));
    rel[16] = 0x01;  /* e_type = ET_REL (1) */
    rel[17] = 0x00;

    elf64_header_t hdr;
    elf_status_t st = elf_validate(rel, sizeof(rel), &hdr);
    ASSERT_EQ(dev, st, ELF_ERR_NOT_EXEC);
}

/* =========================================================================
 * Test: Rejection of wrong architecture
 * ========================================================================= */

static void test_elf_wrong_arch(serial_dev_t *dev) {
    uint8_t arm[64];
    memcpy(arm, mini_elf, sizeof(arm));
    arm[18] = 0x28;  /* e_machine = ARM (0x28) */
    arm[19] = 0x00;

    elf64_header_t hdr;
    elf_status_t st = elf_validate(arm, sizeof(arm), &hdr);
    ASSERT_EQ(dev, st, ELF_ERR_NOT_X86_64);
}

/* =========================================================================
 * Test: Load into a fresh address space
 * ========================================================================= */

static void test_elf_load_segments(serial_dev_t *dev) {
    /* Create a fresh address space. */
    address_space_t as;
    vmm_status_t st = vmm_create_address_space(&as);
    ASSERT_EQ(dev, st, VMM_OK);

    /* Load the mini ELF. */
    elf_load_result_t result;
    elf_status_t est = elf_load(mini_elf, sizeof(mini_elf), &as, &result);
    ASSERT_EQ(dev, est, ELF_OK);

    /* Verify result. */
    ASSERT_EQ(dev, result.entry, 0x400000ULL);
    ASSERT_EQ(dev, result.base, 0x400000ULL);
    ASSERT_EQ(dev, result.phdr_count, 1);

    /* top should be page-aligned and >= base. */
    ASSERT_TRUE(dev, result.top >= result.base);
    ASSERT_TRUE(dev, (result.top & 0xFFF) == 0);

    /* Verify the page is mapped by translating. */
    uint64_t phys = vmm_translate(&as, 0x400000);
    ASSERT_TRUE(dev, phys != 0);

    /* Verify the RET instruction was copied. */
    uint8_t *page = (uint8_t *)pt_phys_to_virt(phys);
    ASSERT_EQ(dev, page[0], 0xC3);

    /* Clean up — free the mapped frame. */
    vmm_unmap_and_free(&as, 0x400000);
    vmm_destroy_address_space(&as);
}

/* =========================================================================
 * Test: Load with zero file size (BSS-only segment)
 * ========================================================================= */

static void test_elf_bss_segment(serial_dev_t *dev) {
    /* Build an ELF with p_filesz=0, p_memsz=0x1000 (BSS). */
    uint8_t bss_elf[120];
    memcpy(bss_elf, mini_elf, sizeof(bss_elf));

    /* Patch program header: filesz=0, memsz=0x1000, flags=RW (6)
     * Program header at offset 0x40 within ELF:
     *   p_flags @ 0x44, p_filesz @ 0x60, p_memsz @ 0x68 */
    bss_elf[0x44] = 0x06;  /* p_flags = PF_R|PF_W */
    bss_elf[0x60] = 0x00;  /* p_filesz = 0 (all 8 bytes zeroed) */
    bss_elf[0x61] = 0x00;
    bss_elf[0x68] = 0x00;  /* p_memsz = 0x1000 */
    bss_elf[0x69] = 0x10;

    address_space_t as;
    vmm_status_t st = vmm_create_address_space(&as);
    ASSERT_EQ(dev, st, VMM_OK);

    elf_load_result_t result;
    elf_status_t est = elf_load(bss_elf, sizeof(bss_elf), &as, &result);
    ASSERT_EQ(dev, est, ELF_OK);
    ASSERT_EQ(dev, result.entry, 0x400000ULL);

    /* The frame should be zeroed. */
    uint64_t phys = vmm_translate(&as, 0x400000);
    ASSERT_TRUE(dev, phys != 0);
    uint8_t *page = (uint8_t *)pt_phys_to_virt(phys);
    ASSERT_EQ(dev, page[0], 0);
    ASSERT_EQ(dev, page[255], 0);

    vmm_unmap_and_free(&as, 0x400000);
    vmm_destroy_address_space(&as);
}

/* =========================================================================
 * Test: Register all ELF tests
 * ========================================================================= */

void test_register_elf(void) {
    test_register("elf_valid_header",   test_elf_valid_header);
    test_register("elf_bad_magic",      test_elf_bad_magic);
    test_register("elf_too_small",      test_elf_too_small);
    test_register("elf_not_executable", test_elf_not_executable);
    test_register("elf_wrong_arch",     test_elf_wrong_arch);
    test_register("elf_load_segments",  test_elf_load_segments);
    test_register("elf_bss_segment",    test_elf_bss_segment);
}
