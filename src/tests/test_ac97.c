#include "../tests/test.h"
#include "../drivers/ac97.h"
#include "../drivers/pci.h"
#include <stdint.h>

static void test_ac97_init_no_crash(serial_dev_t *dev) {
    ac97_init(NULL);
    ASSERT_TRUE(dev, 1);
}

static void test_ac97_found_after_init(serial_dev_t *dev) {
    pci_init(NULL);
    ac97_init(NULL);
    /* In QEMU, AC97 may or may not be present. Just verify no crash. */
    bool found = ac97_found();
    (void)found;
    ASSERT_TRUE(dev, 1);
}

static void test_ac97_volume_no_crash(serial_dev_t *dev) {
    pci_init(NULL);
    ac97_init(NULL);
    uint8_t vol = ac97_get_master_volume();
    (void)vol;
    ASSERT_TRUE(dev, 1);
}

static void test_ac97_set_volume_no_crash(serial_dev_t *dev) {
    pci_init(NULL);
    ac97_init(NULL);
    ac97_set_master_volume(32);
    ASSERT_TRUE(dev, 1);
}

static void test_ac97_mute_unmute_no_crash(serial_dev_t *dev) {
    pci_init(NULL);
    ac97_init(NULL);
    ac97_mute();
    ac97_unmute();
    ASSERT_TRUE(dev, 1);
}

void test_register_ac97(void) {
    test_register("ac97_init_no_crash",       test_ac97_init_no_crash);
    test_register("ac97_found_after_init",     test_ac97_found_after_init);
    test_register("ac97_volume_no_crash",      test_ac97_volume_no_crash);
    test_register("ac97_set_volume_no_crash",  test_ac97_set_volume_no_crash);
    test_register("ac97_mute_unmute_no_crash", test_ac97_mute_unmute_no_crash);
}
