/**
 * @file user_main.c
 * @brief User-mode entry point and helpers.
 *
 * user_task_entry is the C entry point for every ring-3 process.
 * user_task_iretq_trampoline is a kernel-mode trampoline that performs
 * IRETQ to transfer execution from ring 0 to ring 3.
 */

#include "../kernel/syscall.h"
#include "../kernel/gdt.h"
#include "../kernel/task.h"
#include <stdint.h>

/* =========================================================================
 * Inline syscall wrappers for user-mode code
 * ========================================================================= */

static inline long user_syscall1(long num, long a1) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long user_syscall2(long num, long a1, long a2) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long user_syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long user_syscall4(long num, long a1, long a2, long a3, long a4) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(a4)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long user_syscall6(long num, long a1, long a2, long a3,
                                  long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

long user_write(int fd, const void *buf, size_t count) {
    return user_syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

long user_exit(int code) {
    return user_syscall1(SYS_EXIT, code);
}

long user_getpid(void) {
    return user_syscall1(SYS_GETPID, 0);
}

long user_brk(uint64_t addr) {
    return user_syscall1(SYS_BRK, (long)addr);
}

long user_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags) {
    return user_syscall4(SYS_MMAP, (long)addr, (long)length,
                          (long)prot, (long)flags);
}

long user_munmap(uint64_t addr, uint64_t length) {
    return user_syscall2(SYS_MUNMAP, (long)addr, (long)length);
}

long user_execve(const void *elf_data, size_t elf_size) {
    return user_syscall2(SYS_EXECVE, (long)elf_data, (long)elf_size);
}

/* =========================================================================
 * User-mode entry trampoline
 * ========================================================================= */

/**
 * @brief Kernel-mode trampoline that performs IRETQ to ring 3.
 *
 * When a user task is first scheduled, context_switch "returns" here
 * (kernel mode).  We load the IRETQ frame that was built on the
 * kernel stack and execute IRETQ to transfer to user mode.
 *
 * The IRETQ frame layout (5 qwords at current RSP):
 *   [RSP+0]  RIP  = user task entry address
 *   [RSP+8]  CS   = GDT_USER_CS_SEL (0x1B)
 *   [RSP+16] RFLAGS = 0x202 (IF enabled)
 *   [RSP+24] RSP  = user stack top
 *   [RSP+32] SS   = GDT_USER_DS_SEL (0x23)
 */
void user_task_iretq_trampoline(void) {
    __asm__ volatile (
        "iretq"
        : : : "memory"
    );
    /* Should never return. */
    while (1) { __asm__ volatile ("hlt"); }
}

/* =========================================================================
 * First user-mode process
 * ========================================================================= */

/**
 * @brief Entry point for the first user-mode process.
 *
 * Runs in ring 3 (called from user_task_entry in entry.asm).
 * Performs a few simple syscalls to test the mechanism.
 * Then exits cleanly via SYS_EXIT.
 */
void user_task_entry(void) {
    /* Test 1: getpid should return our PID (> 0). */
    long pid = user_getpid();
    (void)pid;

    /* Test 2: write "U" to stdout to prove we're in user mode. */
    user_write(1, "U", 1);

    /* Test 3: exit cleanly. */
    user_exit(0);

    /* Should never reach here. */
    while (1) { __asm__ volatile ("hlt"); }
}
