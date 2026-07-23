#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

#include <stdint.h>

/* Intel SDM Vol.3 §4.7: Triple fault causes immediate CPU reset.
   Kernel panic halts all execution with a register dump before that happens. */

typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cs, ss, ds, es, fs, gs;
    uint64_t cr2;
    uint64_t error_code;
} __attribute__((packed)) panic_regs_t;

void panic_init(void);
void panic_set_serial(void *serial_dev);
void kernel_panic(const char *message, panic_regs_t *regs);
void panic_halt(void);

#endif
