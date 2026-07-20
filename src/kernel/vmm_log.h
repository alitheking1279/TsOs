/**
 * @file vmm_log.h
 * @brief VMM logging interface — compile-time log levels with serial output.
 *
 * Provides the same serial logging style as pmm.c (direct serial_write
 * calls gated through a device pointer) but adds log-level filtering
 * so high-volume debug traces can be compiled out in release builds.
 *
 * Log output format:
 *   [VMM:DBG] ...   — debug traces (every map/unmap/walk step)
 *   [VMM:INF] ...   — informational (init, address space create/destroy)
 *   [VMM:WRN] ...   — warnings (degraded behavior, non-fatal)
 *   [VMM:ERR] ...   — errors (invalid args, OOM, corruption)
 *
 * All output goes through the same serial device used by the PMM.
 * Set via vmm_log_init() at startup; suppressed if serial is NULL.
 */

#ifndef VMM_LOG_H
#define VMM_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Compile-time log level
 *
 * Set VMM_LOG_LEVEL at build time (e.g. -DVMM_LOG_LEVEL=2) to control
 * which log messages are compiled in.  Higher values suppress more output.
 *
 *   0 = DEBUG  (all messages)
 *   1 = INFO   (init, create, destroy, map summary)
 *   2 = WARN   (warnings and errors only)
 *   3 = ERROR  (errors only)
 *   4 = NONE   (all logging compiled out)
 *
 * Default: DEBUG in development, INFO for release.
 * ========================================================================= */

#ifndef VMM_LOG_LEVEL
#define VMM_LOG_LEVEL 0   /* DEBUG — show everything during development */
#endif

#define VMM_LOG_DEBUG  0
#define VMM_LOG_INFO   1
#define VMM_LOG_WARN   2
#define VMM_LOG_ERROR  3
#define VMM_LOG_NONE   4

/* =========================================================================
 * Logging function declarations
 *
 * Implemented in vmm.c.  These are thin wrappers around serial_write
 * that gate output on the current log level and the serial device pointer.
 * ========================================================================= */

/** Initialize the VMM log subsystem with a serial device. */
void vmm_log_init(void *serial_dev);

/** Core log function: outputs "[VMM:XXX] " prefix + user message. */
void vmm_log_write(int level, const char *msg);

/** Print a 64-bit hex value (0xHHHHHHHHHHHHHHHH) to the VMM log. */
void vmm_log_hex64(uint64_t val);

/** Print a decimal unsigned integer to the VMM log. */
void vmm_log_uint64(uint64_t val);

/** Print a single character to the VMM log. */
void vmm_log_char(char c);

/* =========================================================================
 * Convenience macros — compile-time level filtering
 *
 * Usage:
 *   VMM_LOG_DEBUG("MAP: vaddr 0x%lx\r\n", vaddr);   // compile-time gated
 *   VMM_LOG_INFO("Init complete.\r\n");
 *   VMM_LOG_WARN("OOM, retrying...\r\n");
 *   VMM_LOG_ERR("Invalid alignment: 0x%lx\r\n", addr);
 *
 * These wrap vmm_log_write() with the appropriate level constant.
 * The compiler eliminates the entire call when the level is above the
 * configured threshold, producing zero overhead.
 * ========================================================================= */

#if VMM_LOG_LEVEL <= VMM_LOG_DEBUG
  #define VMM_LOG_DBG(msg) vmm_log_write(0, msg)
  #define VMM_LOG_DBG_HEX(val) do { vmm_log_write(0, ""); vmm_log_hex64(val); } while(0)
#else
  #define VMM_LOG_DBG(msg) ((void)0)
  #define VMM_LOG_DBG_HEX(val) ((void)0)
#endif

#if VMM_LOG_LEVEL <= VMM_LOG_INFO
  #define VMM_LOG_INF(msg) vmm_log_write(1, msg)
  #define VMM_LOG_INF_HEX(val) do { vmm_log_write(1, ""); vmm_log_hex64(val); } while(0)
#else
  #define VMM_LOG_INF(msg) ((void)0)
  #define VMM_LOG_INF_HEX(val) ((void)0)
#endif

#if VMM_LOG_LEVEL <= VMM_LOG_WARN
  #define VMM_LOG_WRN(msg) vmm_log_write(2, msg)
#else
  #define VMM_LOG_WRN(msg) ((void)0)
#endif

#if VMM_LOG_LEVEL <= VMM_LOG_ERROR
  #define VMM_LOG_ERR(msg) vmm_log_write(3, msg)
#else
  #define VMM_LOG_ERR(msg) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* VMM_LOG_H */
