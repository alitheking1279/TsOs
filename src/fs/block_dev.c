/**
 * @file block_dev.c
 * @brief Block device abstraction layer — implementation.
 */

#include "block_dev.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Device Table
 * ========================================================================= */

/** Global device table — one slot per registered block device. */
static block_device_t g_devices[BLOCK_DEV_MAX];

/** Serial device for logging. */
static serial_dev_t *g_bd_serial = NULL;

/* =========================================================================
 * Logging Helpers
 * ========================================================================= */

static void bd_log(const char *msg) {
    if (g_bd_serial) serial_write_string(g_bd_serial, msg);
}

/* =========================================================================
 * Initialization
 * ========================================================================= */

/**
 * @brief Initialize the block device subsystem.
 *
 * Zeros the device table and stores the serial device for logging.
 * Must be called before any block_dev_register() calls.
 *
 * @param serial_dev  Serial device for logging.  May be NULL.
 */
void block_dev_init(void *serial_dev) {
    g_bd_serial = (serial_dev_t *)serial_dev;
    memset(g_devices, 0, sizeof(g_devices));
    bd_log("[BLOCK] Block device layer initialized.\r\n");
}

/* =========================================================================
 * Registration
 * ========================================================================= */

block_status_t block_dev_register(block_device_t *dev, uint8_t *dev_id) {
    if (!dev) return BLOCK_ERR_NULL_DEV;
    if (!dev_id) return BLOCK_ERR_NULL_BUF;
    if (!dev->read_sector || !dev->write_sector) return BLOCK_ERR_NULL_DEV;

    for (uint8_t i = 0; i < BLOCK_DEV_MAX; i++) {
        if (!g_devices[i].present) {
            g_devices[i] = *dev;
            g_devices[i].present = true;
            *dev_id = i;

            bd_log("[BLOCK] Registered device ID=");
            /* Simple decimal print. */
            char buf[4];
            buf[0] = '0' + i;
            buf[1] = '\0';
            bd_log(buf);
            bd_log("\r\n");

            return BLOCK_OK;
        }
    }

    return BLOCK_ERR_TOO_MANY;
}

block_status_t block_dev_unregister(uint8_t dev_id) {
    if (dev_id >= BLOCK_DEV_MAX) return BLOCK_ERR_NOT_FOUND;
    if (!g_devices[dev_id].present) return BLOCK_ERR_NOT_FOUND;

    g_devices[dev_id].present = false;
    memset(&g_devices[dev_id], 0, sizeof(block_device_t));
    return BLOCK_OK;
}

block_device_t *block_dev_get(uint8_t dev_id) {
    if (dev_id >= BLOCK_DEV_MAX) return NULL;
    if (!g_devices[dev_id].present) return NULL;
    return &g_devices[dev_id];
}

/* =========================================================================
 * Sector I/O
 * ========================================================================= */

block_status_t block_dev_read(uint8_t dev_id, uint32_t lba, uint32_t count, void *buf) {
    if (dev_id >= BLOCK_DEV_MAX) return BLOCK_ERR_NOT_FOUND;
    if (!g_devices[dev_id].present) return BLOCK_ERR_NOT_FOUND;
    if (!buf) return BLOCK_ERR_NULL_BUF;
    if (count == 0) return BLOCK_ERR_INVALID;

    block_device_t *dev = &g_devices[dev_id];
    if (!dev->read_sector) return BLOCK_ERR_NULL_DEV;

    uint8_t *ptr = (uint8_t *)buf;
    uint32_t sector_sz = dev->sector_size ? dev->sector_size : BLOCK_DEV_SECTOR_SIZE;

    for (uint32_t s = 0; s < count; s++) {
        block_status_t st = dev->read_sector(dev_id, lba + s, ptr);
        if (st != BLOCK_OK) return st;
        ptr += sector_sz;
    }

    return BLOCK_OK;
}

block_status_t block_dev_write(uint8_t dev_id, uint32_t lba, uint32_t count, const void *buf) {
    if (dev_id >= BLOCK_DEV_MAX) return BLOCK_ERR_NOT_FOUND;
    if (!g_devices[dev_id].present) return BLOCK_ERR_NOT_FOUND;
    if (!buf) return BLOCK_ERR_NULL_BUF;
    if (count == 0) return BLOCK_ERR_INVALID;

    block_device_t *dev = &g_devices[dev_id];
    if (!dev->write_sector) return BLOCK_ERR_NULL_DEV;

    const uint8_t *ptr = (const uint8_t *)buf;
    uint32_t sector_sz = dev->sector_size ? dev->sector_size : BLOCK_DEV_SECTOR_SIZE;

    for (uint32_t s = 0; s < count; s++) {
        block_status_t st = dev->write_sector(dev_id, lba + s, ptr);
        if (st != BLOCK_OK) return st;
        ptr += sector_sz;
    }

    return BLOCK_OK;
}

/* =========================================================================
 * Geometry Queries
 * ========================================================================= */

uint32_t block_dev_sector_size(uint8_t dev_id) {
    if (dev_id >= BLOCK_DEV_MAX) return 0;
    if (!g_devices[dev_id].present) return 0;
    return g_devices[dev_id].sector_size ? g_devices[dev_id].sector_size
                                         : BLOCK_DEV_SECTOR_SIZE;
}

uint64_t block_dev_total_sectors(uint8_t dev_id) {
    if (dev_id >= BLOCK_DEV_MAX) return 0;
    if (!g_devices[dev_id].present) return 0;
    return g_devices[dev_id].total_sectors;
}
