#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCI_CONFIG_ADDR   0xCF8
#define PCI_CONFIG_DATA   0xCFC
#define PCI_MAX_SLOTS     32
#define PCI_MAX_DEVICES   32

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class, subclass, prog_if, revision;
    uint8_t  irq_line;
    uint32_t bar0, bar1;
} pci_dev_t;

void        pci_init(void *serial_dev);
uint32_t    pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void        pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
pci_dev_t  *pci_find_device(uint16_t vendor, uint16_t device);
pci_dev_t  *pci_find_device_by_class(uint8_t class_code, uint8_t subclass);
void        pci_enable_bus_mastering(pci_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_PCI_H */
