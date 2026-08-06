#include "../tests/test.h"
#include "../drivers/pcspk.h"
#include "../drivers/pit.h"
#include "../kernel/timer.h"
#include <stdint.h>

static void test_pcspk_init_no_crash(serial_dev_t *dev) {
    pcspk_init(NULL);
    ASSERT_TRUE(dev, 1);
}

static void test_pcspk_mute_no_crash(serial_dev_t *dev) {
    pcspk_mute();
    ASSERT_TRUE(dev, 1);
}

static void test_pcspk_beep_zero_freq_no_crash(serial_dev_t *dev) {
    pcspk_beep(0, 10);
    ASSERT_TRUE(dev, 1);
}

static void test_pcspk_beep_huge_freq_no_crash(serial_dev_t *dev) {
    pcspk_beep(999999, 10);
    ASSERT_TRUE(dev, 1);
}

static void test_pcspk_click_no_crash(serial_dev_t *dev) {
    pcspk_click();
    ASSERT_TRUE(dev, 1);
}

void test_register_pcspk(void) {
    test_register("pcspk_init_no_crash",       test_pcspk_init_no_crash);
    test_register("pcspk_mute_no_crash",       test_pcspk_mute_no_crash);
    test_register("pcspk_beep_zero_freq",      test_pcspk_beep_zero_freq_no_crash);
    test_register("pcspk_beep_huge_freq",      test_pcspk_beep_huge_freq_no_crash);
    test_register("pcspk_click_no_crash",      test_pcspk_click_no_crash);
}
