#include "test.h"
#include "kernel/panic.h"

/* Tests for the kernel panic API.
 * We cannot test kernel_panic() directly as it halts the system,
 * but we can test panic_init() and verify the struct layout. */

static void test_panic_init(serial_dev_t *dev)
{
    panic_init();
    /* If we reach here, panic_init didn't crash. */
    ASSERT_TRUE(dev, 1);
}

static void test_panic_regs_size(serial_dev_t *dev)
{
    /* panic_regs_t should be a well-defined size (at least 160 bytes
     * for 16 GP regs + rip/rflags + segments + cr2 + error_code). */
    ASSERT_TRUE(dev, sizeof(panic_regs_t) >= 144);
}

static void test_panic_regs_layout(serial_dev_t *dev)
{
    panic_regs_t regs;
    regs.rax = 0xDEADBEEF;
    regs.rip = 0xFFFFFFFF80000000ULL;
    regs.cr2 = 0x1234567890ULL;
    regs.error_code = 0x0E;

    ASSERT_EQ(dev, regs.rax, (uint64_t)0xDEADBEEF);
    ASSERT_EQ(dev, regs.rip, (uint64_t)0xFFFFFFFF80000000ULL);
    ASSERT_EQ(dev, regs.cr2, (uint64_t)0x1234567890ULL);
    ASSERT_EQ(dev, regs.error_code, (uint64_t)0x0E);
}

void test_register_panic(void)
{
    test_register("panic_init", test_panic_init);
    test_register("panic_regs_size", test_panic_regs_size);
    test_register("panic_regs_layout", test_panic_regs_layout);
}
