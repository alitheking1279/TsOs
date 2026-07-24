/**
 * @file block_dev.h
 * @brief Block device abstraction layer — hardware-independent sector I/O.
 *
 * Provides a uniform interface for reading and writing disk sectors
 * regardless of the underlying hardware (ATA PIO, ATA DMA, AHCI, etc.).
 *
 * Design:
 *   - Up to 8 block devices can be registered simultaneously.
 *   - Each device is identified by a uint8_t device ID (0-7).
 *   - Each device provides function pointers for read/write operations.
 *   - Multi-sector operations are decomposed into single-sector calls
 *     by the block_dev_read/write helpers.
 *   - The block device layer is stateless — it does not cache data.
 *     Use the buffer cache (bcache) for caching.
 *
 * The block device layer sits between:
 *   ATA driver (hardware)  ←  block_dev  →  VFS / ext2 (filesystem)
 */

#ifndef FS_BLOCK_DEV_H
#define FS_BLOCK_DEV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Maximum number of block devices that can be registered. */
#define BLOCK_DEV_MAX        16

/** Standard sector size (512 bytes for ATA, 4096 for some modern drives). */
#define BLOCK_DEV_SECTOR_SIZE 512

/* =========================================================================
 * Status Codes
 * ========================================================================= */

typedef enum {
    BLOCK_OK            =  0,
    BLOCK_ERR_NOT_FOUND = -1,  /* Device ID not registered */
    BLOCK_ERR_NULL_DEV  = -2,  /* Device has NULL function pointers */
    BLOCK_ERR_IO        = -3,  /* Underlying hardware I/O error */
    BLOCK_ERR_INVALID   = -4,  /* Invalid sector number or count */
    BLOCK_ERR_NULL_BUF  = -5,  /* Buffer pointer is NULL */
    BLOCK_ERR_TOO_MANY  = -6,  /* BLOCK_DEV_MAX devices already registered */
} block_status_t;

/* =========================================================================
 * Block Device Structure
 * ========================================================================= */

/**
 * @brief Function pointer type for reading a single sector.
 *
 * @param dev_id   Device ID of the block device.
 * @param lba      Logical Block Address of the sector to read.
 * @param buf      Destination buffer (at least sector_size bytes).
 * @return BLOCK_OK on success, or an error code.
 */
typedef block_status_t (*block_read_fn)(uint8_t dev_id, uint32_t lba, void *buf);

/**
 * @brief Function pointer type for writing a single sector.
 *
 * @param dev_id   Device ID of the block device.
 * @param lba      Logical Block Address of the sector to write.
 * @param buf      Source buffer (at least sector_size bytes).
 * @return BLOCK_OK on success, or an error code.
 */
typedef block_status_t (*block_write_fn)(uint8_t dev_id, uint32_t lba, const void *buf);

/**
 * @brief Describes a single block device — its I/O callbacks and geometry.
 *
 * Callers register a block_device_t via block_dev_register() and receive
 * a device ID.  The device is then accessed via block_dev_read/write.
 */
typedef struct block_device {
    block_read_fn   read_sector;    /**< Read one sector from the device. */
    block_write_fn  write_sector;   /**< Write one sector to the device. */
    uint32_t        sector_size;    /**< Sector size in bytes (typically 512). */
    uint64_t        total_sectors;  /**< Total number of sectors on the device. */
    bool            present;        /**< Whether this slot is occupied. */
} block_device_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the block device subsystem.
 *
 * Zeros the device table and stores the serial device for logging.
 * Must be called before any block_dev_register() calls.
 *
 * @param serial_dev  Serial device for logging.  May be NULL.
 */
void block_dev_init(void *serial_dev);

/**
 * @brief Register a block device and receive a device ID.
 *
 * Finds the first free slot in the device table and populates it with
 * the provided function pointers and geometry.  The caller owns the
 * block_device_t and must keep it alive while the device is in use.
 *
 * @param dev            Pointer to the block_device_t to register.
 * @param[out] dev_id    Receives the assigned device ID (0-7).
 * @return BLOCK_OK on success, BLOCK_ERR_TOO_MANY if all slots are full.
 */
block_status_t block_dev_register(block_device_t *dev, uint8_t *dev_id);

/**
 * @brief Unregister a block device, freeing its slot.
 *
 * @param dev_id  Device ID to unregister.
 * @return BLOCK_OK on success, BLOCK_ERR_NOT_FOUND if the ID is not registered.
 */
block_status_t block_dev_unregister(uint8_t dev_id);

/**
 * @brief Look up a registered block device by its device ID.
 *
 * @param dev_id  Device ID (0-7).
 * @return Pointer to the device, or NULL if not registered.
 */
block_device_t *block_dev_get(uint8_t dev_id);

/**
 * @brief Read one or more contiguous sectors from a block device.
 *
 * Decomposes a multi-sector read into single-sector calls through the
 * device's read_sector function pointer.  Buffer must be at least
 * count * sector_size bytes.
 *
 * @param dev_id  Device ID.
 * @param lba     Starting Logical Block Address.
 * @param count   Number of sectors to read (must be > 0).
 * @param buf     Destination buffer.
 * @return BLOCK_OK on success, or the first error encountered.
 */
block_status_t block_dev_read(uint8_t dev_id, uint32_t lba, uint32_t count, void *buf);

/**
 * @brief Write one or more contiguous sectors to a block device.
 *
 * Decomposes a multi-sector write into single-sector calls through the
 * device's write_sector function pointer.  Buffer must be at least
 * count * sector_size bytes.
 *
 * @param dev_id  Device ID.
 * @param lba     Starting Logical Block Address.
 * @param count   Number of sectors to write (must be > 0).
 * @param buf     Source buffer.
 * @return BLOCK_OK on success, or the first error encountered.
 */
block_status_t block_dev_write(uint8_t dev_id, uint32_t lba, uint32_t count, const void *buf);

/**
 * @brief Get the sector size for a registered block device.
 *
 * @param dev_id  Device ID.
 * @return Sector size in bytes, or 0 if device is not registered.
 */
uint32_t block_dev_sector_size(uint8_t dev_id);

/**
 * @brief Get the total sector count for a registered block device.
 *
 * @param dev_id  Device ID.
 * @return Total sectors, or 0 if device is not registered.
 */
uint64_t block_dev_total_sectors(uint8_t dev_id);

#ifdef __cplusplus
}
#endif

#endif /* FS_BLOCK_DEV_H */
