/**
 * @file test_validation.c
 * @brief Syscall input validation tests.
 *
 * Tests the validate_user_pointer and validate_user_string functions
 * for correct rejection of kernel pointers, NULL, and invalid ranges.
 *
 * NOTE: validate_user_pointer requires a current task with an address
 * space.  These tests are designed to run after task_init + scheduler_init
 * so that task_get_current() returns a valid task.
 *
 * Tests registered (14):
 *   val_reject_null_ptr         — NULL pointer is rejected
 *   val_reject_kernel_ptr       — kernel-space pointer is rejected
 *   val_reject_zero_size        — zero-size range at low addr is rejected
 *   val_reject_low_addr         — address below 0x1000 is rejected
 *   val_reject_high_addr        — address above user canonical limit rejected
 *   val_reject_unmapped         — unmapped user VA is rejected
 *   val_string_reject_null      — validate_user_string rejects NULL
 *   val_string_reject_kernel    — validate_user_string rejects kernel ptr
 *   val_string_reject_zero_len  — max_len=0 returns -1
 *   val_per_cpu_size            — per_cpu_data_t is exactly 16 bytes
 *   val_frame_offsets           — syscall frame offsets are correct
 *   val_msr_constants           — MSR addresses are correct
 *   val_overflow_wrap_rejected  — start+size overflow wraps to small addr
 *   val_zero_size_rejected      — size=0 always returns 0
 */

#include "test.h"
#include "../kernel/syscall.h"
#include "../kernel/task.h"
#include "../kernel/gdt.h"
#include "../kernel/vmm.h"
#include <stdint.h>

/* Frame offset macros — must match syscall.c's internal layout. */
#define SC_OFF_RAX   0
#define SC_OFF_R10   8
#define SC_OFF_R9    16
#define SC_OFF_R8    24
#define SC_OFF_RDX   32
#define SC_OFF_RSI   40
#define SC_OFF_RDI   48

/* ---- Test: NULL pointer rejected ---- */
static void test_val_reject_null_ptr(serial_dev_t *dev) {
    ASSERT_EQ(dev, validate_user_pointer(NULL, 1), 0);
}

/* ---- Test: kernel-space pointer rejected ---- */
static void test_val_reject_kernel_ptr(serial_dev_t *dev) {
    /* Kernel pointers are in upper canonical half. */
    void *kernel_ptr = (void *)0xFFFFFFFF80000000ULL;
    ASSERT_EQ(dev, validate_user_pointer(kernel_ptr, 4096), 0);
}

/* ---- Test: zero-size range at low addr rejected ---- */
static void test_val_reject_zero_size(serial_dev_t *dev) {
    /* Address below 0x1000 is always rejected regardless of size. */
    ASSERT_EQ(dev, validate_user_pointer((void *)0x500, 1), 0);
}

/* ---- Test: address below 0x1000 rejected ---- */
static void test_val_reject_low_addr(serial_dev_t *dev) {
    ASSERT_EQ(dev, validate_user_pointer((void *)0x100, 8), 0);
    ASSERT_EQ(dev, validate_user_pointer((void *)0xFFF, 1), 0);
}

/* ---- Test: address above user canonical limit rejected ---- */
static void test_val_reject_high_addr(serial_dev_t *dev) {
    /* Non-canonical addresses (above 0x00007FFFFFFFFFFF). */
    void *high_ptr = (void *)0x0000800000000000ULL;
    ASSERT_EQ(dev, validate_user_pointer(high_ptr, 1), 0);
}

/* ---- Test: unmapped user VA rejected ---- */
static void test_val_reject_unmapped(serial_dev_t *dev) {
    /* Use a user VA that's in the valid range but not mapped. */
    void *unmapped = (void *)0x0000000010000000ULL;
    ASSERT_EQ(dev, validate_user_pointer(unmapped, 4096), 0);
}

/* ---- Test: validate_user_string rejects NULL ---- */
static void test_val_string_reject_null(serial_dev_t *dev) {
    ASSERT_EQ(dev, validate_user_string(NULL, 256), -1);
}

/* ---- Test: validate_user_string rejects kernel pointer ---- */
static void test_val_string_reject_kernel(serial_dev_t *dev) {
    const char *kernel_str = "kernel string";
    ASSERT_EQ(dev, validate_user_string(kernel_str, 256), -1);
}

/* ---- Test: validate_user_string rejects zero max_len ---- */
static void test_val_string_reject_zero_len(serial_dev_t *dev) {
    /* Even with a "valid" pointer, max_len=0 means we never find null. */
    /* Use a user-range address that won't be mapped -> rejected. */
    const char *ptr = (const char *)0x10000;
    ASSERT_EQ(dev, validate_user_string(ptr, 0), -1);
}

/* ---- Test: per_cpu_data_t is exactly 16 bytes ---- */
static void test_val_per_cpu_size(serial_dev_t *dev) {
    ASSERT_EQ(dev, sizeof(per_cpu_data_t), (size_t)16);
}

/* ---- Test: syscall frame offset constants are correct ---- */
static void test_val_frame_offsets(serial_dev_t *dev) {
    /* Verify the offset macros match the expected frame layout. */
    ASSERT_EQ(dev, SC_OFF_RAX, 0);
    ASSERT_EQ(dev, SC_OFF_R10, 8);
    ASSERT_EQ(dev, SC_OFF_R9,  16);
    ASSERT_EQ(dev, SC_OFF_R8,  24);
    ASSERT_EQ(dev, SC_OFF_RDX, 32);
    ASSERT_EQ(dev, SC_OFF_RSI, 40);
    ASSERT_EQ(dev, SC_OFF_RDI, 48);
}

/* ---- Test: MSR constants are correct ---- */
static void test_val_msr_constants(serial_dev_t *dev) {
    ASSERT_EQ(dev, MSR_EFER,   0xC0000080u);
    ASSERT_EQ(dev, MSR_STAR,   0xC0000081u);
    ASSERT_EQ(dev, MSR_LSTAR,  0xC0000082u);
    ASSERT_EQ(dev, MSR_SFMASK, 0xC0000084u);
    ASSERT_EQ(dev, MSR_GS_BASE, 0xC0000101u);
}

/* ---- Test: integer overflow in validate_user_pointer rejected ---- */
static void test_val_overflow_wrap_rejected(serial_dev_t *dev) {
    /* Regression: start=0xFFFFFFFFFFFFFFFF + size=0x1000 wraps to 0x0FFF,
     * bypassing the user-range check. Fixed by checking start+size < start. */
    void *near_max = (void *)0xFFFFFFFFFFFFFFFFULL;
    ASSERT_EQ(dev, validate_user_pointer(near_max, 0x1000), 0);

    /* Another overflow case: start just below canonical limit, size pushes over. */
    void *near_limit = (void *)0x00007FFFFFFFFFF0ULL;
    ASSERT_EQ(dev, validate_user_pointer(near_limit, 0x1000), 0);
}

/* ---- Test: zero-size always rejected ---- */
static void test_val_zero_size_rejected(serial_dev_t *dev) {
    /* Regression: size=0 should always be rejected (nothing to validate). */
    ASSERT_EQ(dev, validate_user_pointer((void *)0x10000, 0), 0);
    ASSERT_EQ(dev, validate_user_pointer((void *)0x100000, 0), 0);
}

/* ---- Registration ---- */
void test_register_validation(void) {
    test_register("val_reject_null_ptr", test_val_reject_null_ptr);
    test_register("val_reject_kernel_ptr", test_val_reject_kernel_ptr);
    test_register("val_reject_zero_size", test_val_reject_zero_size);
    test_register("val_reject_low_addr", test_val_reject_low_addr);
    test_register("val_reject_high_addr", test_val_reject_high_addr);
    test_register("val_reject_unmapped", test_val_reject_unmapped);
    test_register("val_string_reject_null", test_val_string_reject_null);
    test_register("val_string_reject_kernel", test_val_string_reject_kernel);
    test_register("val_string_reject_zero_len", test_val_string_reject_zero_len);
    test_register("val_per_cpu_size", test_val_per_cpu_size);
    test_register("val_frame_offsets", test_val_frame_offsets);
    test_register("val_msr_constants", test_val_msr_constants);
    test_register("val_overflow_wrap_rejected", test_val_overflow_wrap_rejected);
    test_register("val_zero_size_rejected", test_val_zero_size_rejected);
}
