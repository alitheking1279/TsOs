/**
 * @file test_syscall.c
 * @brief Syscall infrastructure tests — dispatch table, init, selectors.
 *
 * Tests registered (10):
 *   sc_sys_count            — SYS_COUNT == 7
 *   sc_syscall_numbers      — all syscall numbers are sequential
 *   sc_dispatch_table_size  — dispatch table has SYS_COUNT entries
 *   sc_user_cs_selector     — GDT_USER_CS_SEL == 0x1B
 *   sc_user_ds_selector     — GDT_USER_DS_SEL == 0x23
 *   sc_kernel_cs_selector   — GDT_KERNEL_CS_SEL == 0x08
 *   sc_kernel_ds_selector   — GDT_KERNEL_DS_SEL == 0x10
 *   sc_tss_selector         — GDT_TSS_SEL == 0x28
 *   sc_entry_addr_valid     — syscall_entry_addr is non-zero after init
 *   sc_cpu_has_sce          — EFER.SCE is set (read MSR to verify)
 */

#include "test.h"
#include "../kernel/syscall.h"
#include "../kernel/gdt.h"
#include "../kernel/task.h"
#include <stdint.h>

/* ---- Test: SYS_COUNT is 60 ---- */
static void test_sc_sys_count(serial_dev_t *dev) {
    ASSERT_EQ(dev, SYS_COUNT, 62);
}

/* ---- Test: syscall numbers are sequential starting at 0 ---- */
static void test_sc_syscall_numbers(serial_dev_t *dev) {
    ASSERT_EQ(dev, SYS_WRITE,   0);
    ASSERT_EQ(dev, SYS_READ,    1);
    ASSERT_EQ(dev, SYS_EXIT,    2);
    ASSERT_EQ(dev, SYS_FORK,    3);
    ASSERT_EQ(dev, SYS_GETPID,  4);
    ASSERT_EQ(dev, SYS_WAIT,    5);
    ASSERT_EQ(dev, SYS_GETPPID, 6);
}

/* ---- Test: GDT selectors are correct ---- */
static void test_sc_user_cs_selector(serial_dev_t *dev) {
    ASSERT_EQ(dev, GDT_USER_CS_SEL, 0x1B);
}

static void test_sc_user_ds_selector(serial_dev_t *dev) {
    ASSERT_EQ(dev, GDT_USER_DS_SEL, 0x23);
}

static void test_sc_kernel_cs_selector(serial_dev_t *dev) {
    ASSERT_EQ(dev, GDT_KERNEL_CS_SEL, 0x08);
}

static void test_sc_kernel_ds_selector(serial_dev_t *dev) {
    ASSERT_EQ(dev, GDT_KERNEL_DS_SEL, 0x10);
}

static void test_sc_tss_selector(serial_dev_t *dev) {
    ASSERT_EQ(dev, GDT_TSS_SEL, 0x28);
}

/* ---- Test: syscall_entry_addr is valid after init ---- */
static void test_sc_entry_addr_valid(serial_dev_t *dev) {
    /* syscall_entry_addr is set by syscall_init().  If tests run
     * after main.c calls syscall_init, this should be non-zero. */
    ASSERT_TRUE(dev, syscall_entry_addr != 0);
}

/* ---- Test: EFER.SCE bit is set (read MSR) ---- */
static void test_sc_cpu_has_sce(serial_dev_t *dev) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(MSR_EFER));
    uint64_t efer = ((uint64_t)high << 32) | low;
    /* Bit 0 of EFER is SCE (SYSCALL Enable). */
    ASSERT_TRUE(dev, (efer & 1) != 0);
}

/* ---- Test: dispatch table has correct number of entries ---- */
/* We can't directly access the static table, but we can verify
 * the constant used to size it matches SYS_COUNT. */
static void test_sc_dispatch_table_size(serial_dev_t *dev) {
    /* The dispatch table in syscall.c is syscall_table[SYS_COUNT].
     * Verify SYS_COUNT is what we expect for the table bounds. */
    ASSERT_TRUE(dev, SYS_COUNT >= 7);
    ASSERT_TRUE(dev, SYS_COUNT <= 128);  /* sanity check */
}

/* ---- Registration ---- */
void test_register_syscall(void) {
    test_register("sc_sys_count", test_sc_sys_count);
    test_register("sc_syscall_numbers", test_sc_syscall_numbers);
    test_register("sc_dispatch_table_size", test_sc_dispatch_table_size);
    test_register("sc_user_cs_selector", test_sc_user_cs_selector);
    test_register("sc_user_ds_selector", test_sc_user_ds_selector);
    test_register("sc_kernel_cs_selector", test_sc_kernel_cs_selector);
    test_register("sc_kernel_ds_selector", test_sc_kernel_ds_selector);
    test_register("sc_tss_selector", test_sc_tss_selector);
    test_register("sc_entry_addr_valid", test_sc_entry_addr_valid);
    test_register("sc_cpu_has_sce", test_sc_cpu_has_sce);
}
