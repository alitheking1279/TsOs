#include "../tests/test.h"
#include "../drivers/pci.h"
#include <stdint.h>

static void test_pci_init_no_crash(serial_dev_t *dev) {
    pci_init(NULL);
    ASSERT_TRUE(dev, 1);
}

static void test_pci_find_nonexistent(serial_dev_t *dev) {
    pci_init(NULL);
    pci_dev_t *d = pci_find_device(0xFFFF, 0xFFFF);
    ASSERT_NULL(dev, d);
}

static void test_pci_find_class_nonexistent(serial_dev_t *dev) {
    pci_init(NULL);
    pci_dev_t *d = pci_find_device_by_class(0xFF, 0xFF);
    ASSERT_NULL(dev, d);
}

static void test_pci_read_config_returns_value(serial_dev_t *dev) {
    uint32_t val = pci_read_config_dword(0, 0, 0, 0x00);
    (void)val;
    ASSERT_TRUE(dev, 1);
}

void test_register_pci(void) {
    test_register("pci_init_no_crash",         test_pci_init_no_crash);
    test_register("pci_find_nonexistent",      test_pci_find_nonexistent);
    test_register("pci_find_class_nonexist",   test_pci_find_class_nonexistent);
    test_register("pci_read_config_dword",     test_pci_read_config_returns_value);
}
