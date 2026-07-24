/**
 * @file ata.c
 * @brief ATA PIO mode driver — polling-based sector read/write.
 *
 * Implementation notes:
 *   - Uses 28-bit LBA addressing for maximum 128 GiB address space.
 *   - All register accesses are via inb/outb (x86 port I/O).
 *   - Status polling uses a bounded loop (ATA_TIMEOUT iterations) to
 *     avoid infinite hangs on non-existent drives.
 *   - The 400ns delay between command and status read is implemented as
 *     4 reads from the alternate status register (each ~100ns on x86).
 *   - Writes include a CACHE FLUSH (0xE7) after the data transfer to
 *     ensure QEMU's IDE emulation commits data to the backing image.
 *   - All functions are synchronous and disable interrupts during the
 *     transfer (ATA PIO is not interrupt-driven).
 */

#include "ata.h"
#include "portio.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Static State
 * ========================================================================= */

/** Serial device for logging (set during ata_init). */
static serial_dev_t *g_ata_serial = NULL;

/** Whether the driver has been initialized and a drive is present. */
static bool g_ata_initialized = false;

/* =========================================================================
 * Logging Helpers
 * ========================================================================= */

static void ata_log(const char *msg) {
    if (g_ata_serial) serial_write_string(g_ata_serial, msg);
}

static void ata_log_hex(const char *label, uint32_t val) {
    static const char h[] = "0123456789ABCDEF";
    if (!g_ata_serial) return;
    serial_write_string(g_ata_serial, label);
    serial_write_string(g_ata_serial, "0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_write_char(g_ata_serial, h[(val >> i) & 0xF]);
}

/* =========================================================================
 * Low-Level Helpers
 * ========================================================================= */

/**
 * @brief Perform a 400ns delay by reading the alternate status register 4 times.
 *
 * Intel 82371AB PIIX4 timing: t0 = 600ns for command cycle.
 * Each inb() takes ~100ns on x86, so 4 reads ≈ 400ns.
 * This is required after writing the command register and between
 * certain register access sequences per the ATA specification.
 */
static inline void ata_400ns_delay(void) {
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
}

/**
 * @brief Select a drive on the primary ATA controller.
 *
 * Writes the Drive/Head register with the LBA bits [27:24] in the
 * lower nibble and bit 6 (LBA mode) set.
 *
 * @param lba  Logical Block Address (only bits [27:24] are used).
 */
static void ata_select_drive(uint32_t lba) {
    uint8_t head = (uint8_t)(0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_IO + ATA_DRIVE_HEAD, head);
    ata_400ns_delay();
}

/**
 * @brief Poll the status register until BSY clears, then check for errors.
 *
 * Reads the status register in a tight loop, waiting for:
 *   - BSY (bit 7) to clear
 *   - Either DRQ (bit 3) to set (data ready) or ERR (bit 0) to set (error)
 *
 * @return ATA_OK on success, ATA_ERR_TIMEOUT if BSY never clears within
 *         ATA_TIMEOUT iterations, or ATA_ERR_DEVICE_FAULT on ERR/DF.
 */
static ata_status_t ata_poll(void) {
    uint32_t timeout = ATA_TIMEOUT;

    /* Phase 1: Wait for BSY to clear. */
    while (timeout--) {
        uint8_t status = inb(ATA_PRIMARY_IO + ATA_STATUS);
        if (!(status & ATA_SR_BSY)) {
            /* BSY is clear — check for immediate errors. */
            if (status & ATA_SR_ERR)  return ATA_ERR_DEVICE_FAULT;
            if (status & ATA_SR_DF)   return ATA_ERR_DEVICE_FAULT;
            return ATA_OK;
        }
    }
    return ATA_ERR_TIMEOUT;
}

/**
 * @brief Wait for the drive to signal DRQ (Data Request).
 *
 * Polls until DRQ is set or an error/timeout occurs.
 * Called before reading/writing sector data.
 */
static ata_status_t ata_wait_drq(void) {
    uint32_t timeout = ATA_TIMEOUT;
    while (timeout--) {
        uint8_t status = inb(ATA_PRIMARY_IO + ATA_STATUS);
        if (status & ATA_SR_DRQ)  return ATA_OK;
        if (status & ATA_SR_ERR)  return ATA_ERR_DEVICE_FAULT;
        if (status & ATA_SR_DF)   return ATA_ERR_DEVICE_FAULT;
        if (status & ATA_SR_BSY)  continue;
    }
    return ATA_ERR_TIMEOUT;
}

/**
 * @brief Soft-reset the ATA controller.
 *
 * Writes 0x04 (SRST bit) to the device control register, waits 5ms,
 * then clears the reset.  The drive should become ready within ~2 seconds.
 */
static void ata_soft_reset(void) {
    outb(ATA_PRIMARY_CTRL, 0x04);   /* Set SRST bit */
    ata_400ns_delay();
    outb(ATA_PRIMARY_CTRL, 0x00);   /* Clear SRST bit */
    ata_400ns_delay();
}

/* =========================================================================
 * Public API
 * ========================================================================= */

ata_status_t ata_init(void *serial_dev) {
    g_ata_serial = (serial_dev_t *)serial_dev;
    g_ata_initialized = false;

    ata_log("[ATA] Initializing ATA PIO driver...\r\n");

    /* Step 1: Select drive 0. */
    ata_select_drive(0);
    ata_400ns_delay();

    /* Step 2: Zero sector count register (some controllers require this). */
    outb(ATA_PRIMARY_IO + ATA_SECTOR_COUNT, 0);
    outb(ATA_PRIMARY_IO + ATA_LBA_LO, 0);
    outb(ATA_PRIMARY_IO + ATA_LBA_MID, 0);
    outb(ATA_PRIMARY_IO + ATA_LBA_HI, 0);

    /* Step 3: Send IDENTIFY command. */
    outb(ATA_PRIMARY_IO + ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_400ns_delay();

    /* Step 4: Read initial status. */
    uint8_t status = inb(ATA_PRIMARY_IO + ATA_STATUS);
    if (status == 0) {
        /* Status 0 = no drive present on this channel. */
        ata_log("[ATA] No drive detected (status == 0).\r\n");
        return ATA_ERR_DRIVE_NOT_FOUND;
    }

    /* Step 5: Poll until BSY clears. */
    ata_status_t poll_st = ata_poll();
    if (poll_st != ATA_OK) {
        ata_log("[ATA] IDENTIFY: timeout waiting for drive.\r\n");
        return poll_st;
    }

    /* Step 6: Verify non-zero LBA mid/hi (should be zero for ATA, nonzero for ATAPI). */
    uint8_t lba_mid = inb(ATA_PRIMARY_IO + ATA_LBA_MID);
    uint8_t lba_hi  = inb(ATA_PRIMARY_IO + ATA_LBA_HI);
    if (lba_mid != 0 || lba_hi != 0) {
        ata_log("[ATA] ATAPI device detected — PIO driver supports ATA only.\r\n");
        return ATA_ERR_DRIVE_NOT_FOUND;
    }

    /* Step 7: Read the 256-word identification data and discard it.
     * This clears the DRQ state so the drive is ready for commands. */
    for (int i = 0; i < 256; i++) {
        inb(ATA_PRIMARY_IO + ATA_DATA);
    }

    g_ata_initialized = true;
    ata_log("[ATA] Drive detected and initialized.\r\n");
    return ATA_OK;
}

ata_status_t ata_read_sector(uint32_t lba, uint32_t count, void *buf) {
    if (!g_ata_initialized) return ATA_ERR_DRIVE_NOT_FOUND;
    if (count == 0 || count > ATA_MAX_SECTORS) return ATA_ERR_INVALID_SECTOR_COUNT;
    if (lba > ATA_MAX_LBA_28) return ATA_ERR_INVALID_LBA;
    if (!buf) return ATA_ERR_INVALID_LBA;

    uint8_t *ptr = (uint8_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        uint32_t current_lba = lba + s;
        if (current_lba > ATA_MAX_LBA_28) return ATA_ERR_INVALID_LBA;

        /* Select the drive with the high nibble of the LBA. */
        ata_select_drive(current_lba);

        /* Write the sector count (ATA wraps 0 → 256). */
        outb(ATA_PRIMARY_IO + ATA_SECTOR_COUNT, (uint8_t)(count == 256 ? 0 : count));

        /* Write the 28-bit LBA across three registers. */
        outb(ATA_PRIMARY_IO + ATA_LBA_LO,  (uint8_t)(current_lba & 0xFF));
        outb(ATA_PRIMARY_IO + ATA_LBA_MID, (uint8_t)((current_lba >> 8)  & 0xFF));
        outb(ATA_PRIMARY_IO + ATA_LBA_HI,  (uint8_t)((current_lba >> 16) & 0xFF));

        /* Issue READ SECTORS command. */
        outb(ATA_PRIMARY_IO + ATA_COMMAND, ATA_CMD_READ_SECTORS);

        /* 400ns delay after command write (mandatory per ATA spec). */
        ata_400ns_delay();

        /* Wait for the drive to signal data is ready. */
        ata_status_t st = ata_wait_drq();
        if (st != ATA_OK) return ATA_ERR_READ_FAILED;

        /* Transfer 256 words (512 bytes) from the data register. */
        uint16_t *word_buf = (uint16_t *)ptr;
        for (int i = 0; i < 256; i++) {
            word_buf[i] = inw(ATA_PRIMARY_IO + ATA_DATA);
        }

        /* Read status to clear any pending interrupt state. */
        inb(ATA_PRIMARY_IO + ATA_STATUS);

        ptr += ATA_SECTOR_SIZE;
    }

    return ATA_OK;
}

ata_status_t ata_write_sector(uint32_t lba, uint32_t count, const void *buf) {
    if (!g_ata_initialized) return ATA_ERR_DRIVE_NOT_FOUND;
    if (count == 0 || count > ATA_MAX_SECTORS) return ATA_ERR_INVALID_SECTOR_COUNT;
    if (lba > ATA_MAX_LBA_28) return ATA_ERR_INVALID_LBA;
    if (!buf) return ATA_ERR_INVALID_LBA;

    const uint8_t *ptr = (const uint8_t *)buf;

    for (uint32_t s = 0; s < count; s++) {
        uint32_t current_lba = lba + s;
        if (current_lba > ATA_MAX_LBA_28) return ATA_ERR_INVALID_LBA;

        /* Select the drive. */
        ata_select_drive(current_lba);

        /* Write sector count. */
        outb(ATA_PRIMARY_IO + ATA_SECTOR_COUNT, (uint8_t)(count == 256 ? 0 : count));

        /* Write the 28-bit LBA. */
        outb(ATA_PRIMARY_IO + ATA_LBA_LO,  (uint8_t)(current_lba & 0xFF));
        outb(ATA_PRIMARY_IO + ATA_LBA_MID, (uint8_t)((current_lba >> 8)  & 0xFF));
        outb(ATA_PRIMARY_IO + ATA_LBA_HI,  (uint8_t)((current_lba >> 16) & 0xFF));

        /* Issue WRITE SECTORS command. */
        outb(ATA_PRIMARY_IO + ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

        /* 400ns delay after command. */
        ata_400ns_delay();

        /* Wait for DRQ — drive is ready to accept data. */
        ata_status_t st = ata_wait_drq();
        if (st != ATA_OK) return ATA_ERR_WRITE_FAILED;

        /* Transfer 256 words to the data register. */
        const uint16_t *word_buf = (const uint16_t *)ptr;
        for (int i = 0; i < 256; i++) {
            outw(ATA_PRIMARY_IO + ATA_DATA, word_buf[i]);
        }

        /* Poll for completion — drive sets BSY while flushing. */
        st = ata_poll();
        if (st != ATA_OK) return ATA_ERR_WRITE_FAILED;

        /* Check for write fault after BSY clears. */
        uint8_t status = inb(ATA_PRIMARY_IO + ATA_STATUS);
        if (status & ATA_SR_DF) return ATA_ERR_DEVICE_FAULT;
        if (status & ATA_SR_ERR) return ATA_ERR_WRITE_FAILED;

        ptr += ATA_SECTOR_SIZE;
    }

    /* Flush write cache to ensure data reaches non-volatile storage. */
    outb(ATA_PRIMARY_IO + ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_400ns_delay();
    ata_status_t flush_st = ata_poll();
    if (flush_st != ATA_OK) return ATA_ERR_WRITE_FAILED;

    return ATA_OK;
}

bool ata_is_initialized(void) {
    return g_ata_initialized;
}

const char *ata_status_string(ata_status_t status) {
    switch (status) {
        case ATA_OK:                  return "OK";
        case ATA_ERR_TIMEOUT:         return "TIMEOUT";
        case ATA_ERR_DRIVE_NOT_FOUND: return "DRIVE_NOT_FOUND";
        case ATA_ERR_IDENTIFY_FAILED: return "IDENTIFY_FAILED";
        case ATA_ERR_INVALID_LBA:     return "INVALID_LBA";
        case ATA_ERR_INVALID_SECTOR_COUNT: return "INVALID_SECTOR_COUNT";
        case ATA_ERR_READ_FAILED:     return "READ_FAILED";
        case ATA_ERR_WRITE_FAILED:    return "WRITE_FAILED";
        case ATA_ERR_DEVICE_FAULT:    return "DEVICE_FAULT";
        default:                      return "UNKNOWN";
    }
}
