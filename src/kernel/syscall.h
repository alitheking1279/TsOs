/**
 * @file syscall.h
 * @brief SYSCALL/SYSRET interface — MSR setup, dispatch, and input validation.
 *
 * Uses the AMD64 SYSCALL/SYSRET mechanism (Intel SDM Vol.3A §3.3.1):
 *   - SYSCALL: fast ring-3 → ring-0 transition (no IDT involvement)
 *   - SYSRET:  fast ring-0 → ring-3 return
 *
 * MSR configuration:
 *   EFER.SCE = 1     (enable SYSCALL instruction, set in boot.asm)
 *   STAR      = 0x0013000800000000  (kernel CS=0x08, SS=0x10;
 *                                     user CS=0x1B, SS=0x23)
 *   LSTAR     = &syscall_entry  (kernel entry point)
 *   SFMASK    = 0x200           (clear IF on SYSCALL — disable interrupts)
 *
 * Syscall ABI (Linux-compatible):
 *   RAX = syscall number
 *   RDI, RSI, RDX, R10, R8, R9 = arguments 1-6
 *   Return value in RAX
 *
 * References:
 *   Intel SDM Vol.3A §3.3.1  — SYSCALL/SYSRET
 *   AMD APM Vol.2 §3.3       — SYSCALL/SYSRET in long mode
 */

#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include "isr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Syscall Numbers (Linux ABI subset)
 * ========================================================================= */

#define SYS_WRITE       0
#define SYS_READ        1
#define SYS_EXIT        2
#define SYS_FORK        3
#define SYS_GETPID      4
#define SYS_WAIT        5
#define SYS_GETPPID     6
#define SYS_BRK         12
#define SYS_MMAP        9
#define SYS_MUNMAP      11
#define SYS_EXECVE      59
#define SYS_OPEN        60
#define SYS_CLOSE       61

/** Total number of syscall slots. */
#define SYS_COUNT       62

/** Invalid/unimplemented syscall number. */
#define SYS_INVALID     (-1)

/* =========================================================================
 * Per-CPU Data (single-core: one static instance)
 *
 * Stored at a fixed kernel address. GS-base points here.
 * The syscall entry assembly reads g_kernel_rsp from offset 0
 * to get the current task's kernel stack.
 * ========================================================================= */

typedef struct {
    uint64_t kernel_rsp;    /**< [+0] Current task's kernel stack pointer. */
    uint64_t reserved;      /**< [+8] Reserved for future per-CPU fields. */
} __attribute__((packed)) per_cpu_data_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the SYSCALL/SYSRET mechanism.
 *
 * Programs STAR, LSTAR, and SFMASK MSRs. Sets up GS-base
 * for per-CPU data access from syscall_entry.
 *
 * Must be called after gdt_init() (needs GDT for segment selectors)
 * and after scheduler_init() (needs a valid kernel stack).
 */
void syscall_init(void *serial_dev);

/**
 * @brief C-level syscall dispatcher.
 *
 * Called from syscall_entry.asm with a pointer to the syscall frame.
 * Reads the syscall number from the frame, dispatches to the handler,
 * and stores the return value back into the frame.
 *
 * @param frame  Pointer to the syscall frame on the kernel stack.
 */
void syscall_handler(void *frame);

/**
 * @brief Validate that a user-space pointer range is safe to access.
 *
 * Walks the page tables of the current task to verify:
 *   - Pointer is in user half (lower canonical range)
 *   - Every page in the range is present
 *   - Every page has the USER bit set
 *
 * @param ptr    Start of the range (user-space pointer).
 * @param size   Size of the range in bytes.
 * @return 1 if valid, 0 if invalid.
 */
int validate_user_pointer(const void *ptr, size_t size);

/**
 * @brief Validate a null-terminated user-space string.
 *
 * Walks the page tables to verify every character up to the null
 * terminator is in a valid, user-accessible page.
 *
 * @param str     Pointer to the string (user-space).
 * @param max_len Maximum number of characters to check.
 * @return Length of the string (excluding null) if valid, -1 if invalid.
 */
int validate_user_string(const char *str, size_t max_len);

/* =========================================================================
 * Helpers (used by syscall_entry.asm)
 * ========================================================================= */

/**
 * @brief Update the kernel RSP saved in per-CPU data.
 *
 * Called from the scheduler on every task switch so the syscall
 * entry has a valid kernel stack to use.
 *
 * @param rsp  New kernel stack pointer for the incoming task.
 */
void syscall_set_kernel_rsp(uint64_t rsp);

/* =========================================================================
 * MSR Write Helper
 * ========================================================================= */

/**
 * @brief Write a Model-Specific Register.
 *
 * @param msr   MSR address (e.g. MSR_STAR).
 * @param value 64-bit value to write.
 */
static inline void write_msr(uint32_t msr, uint64_t value) {
    uint32_t low  = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

/**
 * @brief Read a Model-Specific Register.
 *
 * @param msr  MSR address.
 * @return 64-bit value read from the MSR.
 */
static inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_SYSCALL_H */
