/**
 * @file ata.h
 * @brief ATA PIO mode disk driver — primary controller on ports 0x1F0-0x1F7.
 *
 * Provides synchronous (polling) sector read/write using 28-bit LBA
 * addressing.  No interrupts — all data transfer is polled via the
 * status register.
 *
 * Intel 82371AB PIIX4 EISA Controller / ATA PIO Specification:
 *   - Command block:  I/O ports 0x1F0-0x1F7
 *   - Control block:  I/O port 0x3F6 (alternate status / device control)
 *
 * LBA28 addressing scheme (sector-level):
 *   bits [27:24] → Drive/Head register (bits 3:0)
 *   bits [23:16] → Cylinder High register
 *   bits [15:8]  → Cylinder Low register
 *   bits [7:0]   → Sector Number register
 *
 * Each sector is 512 bytes.  Data transfers are 16-bit word-at-a-time
 * through the data register (0x1F0).
 *
 * References:
 *   Intel 82371AB (PIIX4) EISA IDE Controller — Book 1, Ch. 5
 *   ATA PIO Mode 0 Timing — t0 = 600ns, t1 = 165ns, t2 = 290ns
 */

#ifndef DRIVERS_ATA_H
#define DRIVERS_ATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Port Definitions
 * ========================================================================= */

/** Primary ATA command block base (I/O ports 0x1F0-0x1F7). */
#define ATA_PRIMARY_IO      0x1F0

/** Primary ATA control/status block (alternate status at 0x3F6). */
#define ATA_PRIMARY_CTRL    0x3F6

/** Command block register offsets (relative to ATA_PRIMARY_IO). */
#define ATA_DATA            0   /* Data register (R/W, 16-bit) — offset +0 */
#define ATA_ERROR           1   /* Error register (read) — offset +1 */
#define ATA_FEATURES        1   /* Features register (write) — offset +1 */
#define ATA_SECTOR_COUNT    2   /* Sector count register — offset +2 */
#define ATA_LBA_LO          3   /* LBA bits [7:0] (Sector Number) — offset +3 */
#define ATA_LBA_MID         4   /* LBA bits [15:8] (Cylinder Low) — offset +4 */
#define ATA_LBA_HI          5   /* LBA bits [23:16] (Cylinder High) — offset +5 */
#define ATA_DRIVE_HEAD      6   /* Drive/Head register — offset +6 */
#define ATA_STATUS          7   /* Status register (read) — offset +7 */
#define ATA_COMMAND         7   /* Command register (write) — offset +7 */

/* =========================================================================
 * ATA Commands
 * ========================================================================= */

#define ATA_CMD_READ_SECTORS    0x20  /* Read sector(s) with retry */
#define ATA_CMD_WRITE_SECTORS   0x30  /* Write sector(s) with retry */
#define ATA_CMD_IDENTIFY        0xEC  /* Identify drive */
#define ATA_CMD_IDENTIFY_PACKET 0xA1  /* Identify ATAPI device */
#define ATA_CMD_READ_DMA        0xC8  /* Read DMA (not used in PIO mode) */
#define ATA_CMD_CACHE_FLUSH     0xE7  /* Flush write cache */

/* =========================================================================
 * Status Register Bits (offset +7)
 * ========================================================================= */

#define ATA_SR_BSY          0x80  /* Bit 7: Busy — controller executing a command */
#define ATA_SR_DRDY         0x40  /* Bit 6: Drive Ready */
#define ATA_SR_DF           0x20  /* Bit 5: Drive Write Fault */
#define ATA_SR_DSC          0x10  /* Bit 4: Drive Seek Complete */
#define ATA_SR_DRQ          0x08  /* Bit 3: Data Request — data available for R/W */
#define ATA_SR_CORR         0x04  /* Bit 2: Corrected data */
#define ATA_SR_IDX          0x02  /* Bit 1: Index mark */
#define ATA_SR_ERR          0x01  /* Bit 0: Error — see error register */

/* =========================================================================
 * Error Register Bits (offset +1)
 * ========================================================================= */

#define ATA_ER_BBK          0x80  /* Bit 7: Bad Block */
#define ATA_ER_PNF          0x40  /* Bit 6: Predicted Not Found */
#define ATA_ER_ABRT         0x10  /* Bit 4: Aborted Command */
#define ATA_ER_IDNF         0x08  /* Bit 3: ID Not Found */
#define ATA_ER_UNC          0x04  /* Bit 2: Uncorrectable Data Error */
#define ATA_ER_MED          0x02  /* Bit 1: Media Changed */
#define ATA_ER_MCR          0x01  /* Bit 0: Media Change Requested */

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Sector size in bytes (standard ATA sector). */
#define ATA_SECTOR_SIZE     512

/** Maximum number of sectors transferable in a single PIO command (per ATA spec). */
#define ATA_MAX_SECTORS     256

/** Maximum LBA value for 28-bit LBA addressing (2^28 - 1). */
#define ATA_MAX_LBA_28      0x0FFFFFFF

/** Timeout in iterations for status polling (roughly 10 seconds at ~100ns per iteration). */
#define ATA_TIMEOUT         100000000

/* =========================================================================
 * Status Codes
 * ========================================================================= */

typedef enum {
    ATA_OK              =  0,
    ATA_ERR_TIMEOUT     = -1,  /* Drive did not respond within timeout */
    ATA_ERR_DRIVE_NOT_FOUND = -2, /* No drive detected on the bus */
    ATA_ERR_IDENTIFY_FAILED = -3, /* IDENTIFY command failed */
    ATA_ERR_INVALID_LBA = -4,  /* LBA out of range */
    ATA_ERR_INVALID_SECTOR_COUNT = -5, /* Sector count is 0 or exceeds max */
    ATA_ERR_READ_FAILED = -6,  /* Sector read failed */
    ATA_ERR_WRITE_FAILED = -7, /* Sector write failed */
    ATA_ERR_DEVICE_FAULT = -8, /* Drive reported write fault */
} ata_status_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the ATA PIO driver and detect the primary master drive.
 *
 * Issues an ATA IDENTIFY command to determine if a drive is present.
 * Must be called after serial_init() (for logging) and after port I/O
 * is available.  Must be called before any read/write operations.
 *
 * @param serial_dev  Serial device for logging.  May be NULL.
 * @return ATA_OK if a drive is detected, ATA_ERR_TIMEOUT or
 *         ATA_ERR_DRIVE_NOT_FOUND on failure.
 */
ata_status_t ata_init(void *serial_dev);

/**
 * @brief Read one or more sectors from disk.
 *
 * Reads `count` contiguous sectors starting at `lba` into `buf`.
 * `buf` must be at least `count * ATA_SECTOR_SIZE` bytes and must
 * NOT cross a page boundary that would cause a page fault.
 *
 * Uses 28-bit LBA addressing — maximum LBA is 0x0FFFFFFF (128 GiB).
 * Sector count of 256 means 256 sectors (ATA wraps 0 → 256).
 *
 * @param lba    Starting Logical Block Address (0-based).
 * @param count  Number of sectors to read (1-256).
 * @param buf    Destination buffer (must be at least count * 512 bytes).
 * @return ATA_OK on success, or an error code.
 *
 * @pre  ata_init() must have returned ATA_OK.
 * @pre  buf is valid, writable, and large enough.
 * @pre  lba + count - 1 <= ATA_MAX_LBA_28.
 */
ata_status_t ata_read_sector(uint32_t lba, uint32_t count, void *buf);

/**
 * @brief Write one or more sectors to disk.
 *
 * Writes `count` contiguous sectors starting at `lba` from `buf`.
 * The write is followed by a CACHE FLUSH command to ensure data
 * reaches non-volatile storage (important for QEMU's IDE emulation).
 *
 * @param lba    Starting Logical Block Address (0-based).
 * @param count  Number of sectors to write (1-256).
 * @param buf    Source buffer (must be at least count * 512 bytes).
 * @return ATA_OK on success, or an error code.
 *
 * @pre  ata_init() must have returned ATA_OK.
 * @pre  buf is valid, readable, and large enough.
 * @pre  lba + count - 1 <= ATA_MAX_LBA_28.
 */
ata_status_t ata_write_sector(uint32_t lba, uint32_t count, const void *buf);

/**
 * @brief Check whether the ATA driver has been initialized and a drive is present.
 *
 * @return true if ata_init() succeeded, false otherwise.
 */
bool ata_is_initialized(void);

/**
 * @brief Get a human-readable string describing the last ATA error.
 *
 * @param status  ATA status code to convert.
 * @return Pointer to a static string (never NULL).
 */
const char *ata_status_string(ata_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_ATA_H */
