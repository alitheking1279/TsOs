#include "ac97.h"
#include "pci.h"
#include "portio.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <stdbool.h>

static ac97_dev_t g_ac97;
static serial_dev_t *g_ac97_serial = NULL;

static uint16_t nam_read(uint16_t reg) {
    return inw(g_ac97.nam_base + reg);
}

static void nam_write(uint16_t reg, uint16_t val) {
    outw(g_ac97.nam_base + reg, val);
}

void ac97_init(void *serial_dev) {
    g_ac97_serial = (serial_dev_t *)serial_dev;
    g_ac97.initialized = false;
    g_ac97.pci_dev = NULL;
    g_ac97.master_volume = AC97_MAX_VOLUME;

    /* Find AC97 device on PCI bus (class 04, subclass 01). */
    pci_dev_t *dev = pci_find_device_by_class(AC97_PCI_CLASS, AC97_PCI_SUBCLASS);
    if (!dev) {
        if (g_ac97_serial)
            serial_write_string(g_ac97_serial, "[AC97] No device found.\r\n");
        return;
    }

    g_ac97.pci_dev = dev;

    /* BAR0 contains NABM base (bits [15:2]), NAM base is BAR0 + 0x80. */
    uint32_t bar0 = dev->bar0;
    if (bar0 == 0 || bar0 == 0xFFFFFFFF) {
        if (g_ac97_serial)
            serial_write_string(g_ac97_serial, "[AC97] BAR0 invalid.\r\n");
        return;
    }

    g_ac97.nabm_base = (uint16_t)(bar0 & 0xFFFC);
    g_ac97.nam_base  = g_ac97.nabm_base + 0x80;

    /* Cold reset the codec. */
    nam_write(AC97_REG_RESET, 0);

    /* Verify codec is present by reading the extended ID register.
     * A working AC97 codec returns a non-zero value. */
    uint16_t ext_id = nam_read(AC97_REG_EXTENDED_ID);
    if (ext_id == 0) {
        if (g_ac97_serial)
            serial_write_string(g_ac97_serial, "[AC97] Codec not responding.\r\n");
        return;
    }

    g_ac97.initialized = true;
    g_ac97.master_volume = AC97_MAX_VOLUME;

    if (g_ac97_serial)
        serial_write_string(g_ac97_serial, "[AC97] Initialized.\r\n");
}

bool ac97_found(void) {
    return g_ac97.pci_dev != NULL;
}

uint8_t ac97_get_master_volume(void) {
    if (!g_ac97.initialized) return 0;
    uint16_t vol = nam_read(AC97_REG_MASTER_VOL);
    return (uint8_t)(vol & 0x3F);
}

void ac97_set_master_volume(uint8_t vol) {
    if (!g_ac97.initialized) return;
    if (vol > AC97_MAX_VOLUME) vol = AC97_MAX_VOLUME;
    /* Bits 0-5: volume, bits 6-7: reserved (mute bits in some codecs). */
    nam_write(AC97_REG_MASTER_VOL, vol & 0x3F);
    g_ac97.master_volume = vol;
}

void ac97_mute(void) {
    if (!g_ac97.initialized) return;
    uint16_t vol = nam_read(AC97_REG_MASTER_VOL);
    nam_write(AC97_REG_MASTER_VOL, vol | 0x8000);
}

void ac97_unmute(void) {
    if (!g_ac97.initialized) return;
    uint16_t vol = nam_read(AC97_REG_MASTER_VOL);
    nam_write(AC97_REG_MASTER_VOL, vol & ~0x8000);
}
