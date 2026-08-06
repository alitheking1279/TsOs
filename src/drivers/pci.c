#include "pci.h"
#include "portio.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static pci_dev_t g_pci_devices[PCI_MAX_DEVICES];
static int g_pci_count = 0;
static serial_dev_t *g_pci_serial = NULL;

static void pci_log(const char *msg) {
    if (g_pci_serial) serial_write_string(g_pci_serial, msg);
}

static void pci_log_hex(uint32_t val) {
    if (!g_pci_serial) return;
    static const char hex[] = "0123456789ABCDEF";
    char buf[11] = "0x00000000";
    for (int i = 7; i >= 0; i--) {
        buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    serial_write_string(g_pci_serial, buf);
}

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = 0x80000000
        | ((uint32_t)bus  << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = 0x80000000
        | ((uint32_t)bus  << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

static void pci_read_device(pci_dev_t *dev, uint8_t bus, uint8_t slot) {
    uint32_t id = pci_read_config_dword(bus, slot, 0, 0x00);
    dev->vendor_id = id & 0xFFFF;
    dev->device_id = (id >> 16) & 0xFFFF;
    dev->bus = bus;
    dev->slot = slot;
    dev->func = 0;

    uint32_t class_reg = pci_read_config_dword(bus, slot, 0, 0x08);
    dev->revision = class_reg & 0xFF;
    dev->prog_if = (class_reg >> 8) & 0xFF;
    dev->subclass = (class_reg >> 16) & 0xFF;
    dev->class = (class_reg >> 24) & 0xFF;

    uint32_t misc = pci_read_config_dword(bus, slot, 0, 0x3C);
    dev->irq_line = misc & 0xFF;

    dev->bar0 = pci_read_config_dword(bus, slot, 0, 0x10);
    dev->bar1 = pci_read_config_dword(bus, slot, 0, 0x14);
}

void pci_init(void *serial_dev) {
    g_pci_serial = (serial_dev_t *)serial_dev;
    g_pci_count = 0;
    pci_log("[PCI] Scanning bus 0...\r\n");

    for (uint8_t slot = 0; slot < PCI_MAX_SLOTS; slot++) {
        uint32_t id = pci_read_config_dword(0, slot, 0, 0x00);
        uint16_t vendor = id & 0xFFFF;
        if (vendor == 0xFFFF) continue;

        pci_dev_t *dev = &g_pci_devices[g_pci_count];
        pci_read_device(dev, 0, slot);

        pci_log("[PCI] Found device: vendor=");
        pci_log_hex(dev->vendor_id);
        pci_log(" device=");
        pci_log_hex(dev->device_id);
        pci_log(" class=");
        pci_log_hex(dev->class);
        pci_log(".");
        pci_log_hex(dev->subclass);
        pci_log("\r\n");

        g_pci_count++;
        if (g_pci_count >= PCI_MAX_DEVICES) break;
    }

    char buf[40] = "[PCI] Scan complete. Found ";
    int i = 25;
    if (g_pci_count == 0) { buf[i++] = '0'; }
    else {
        int n = g_pci_count, j = 0;
        char tmp[8];
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) { buf[i++] = tmp[--j]; }
    }
    buf[i++] = ' ';
    buf[i++] = 'd';
    buf[i++] = 'e';
    buf[i++] = 'v';
    buf[i++] = 'i';
    buf[i++] = 'c';
    buf[i++] = 'e';
    buf[i++] = 's';
    buf[i++] = '.';
    buf[i++] = '\r';
    buf[i++] = '\n';
    buf[i] = '\0';
    pci_log(buf);
}

pci_dev_t *pci_find_device(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < g_pci_count; i++) {
        if (g_pci_devices[i].vendor_id == vendor && g_pci_devices[i].device_id == device)
            return &g_pci_devices[i];
    }
    return (void *)0;
}

pci_dev_t *pci_find_device_by_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < g_pci_count; i++) {
        if (g_pci_devices[i].class == class_code && g_pci_devices[i].subclass == subclass)
            return &g_pci_devices[i];
    }
    return (void *)0;
}

void pci_enable_bus_mastering(pci_dev_t *dev) {
    if (!dev) return;
    uint32_t cmd = pci_read_config_dword(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x04;
    pci_write_config_dword(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
