#include "kernel/panic.h"
#include "../lib/print.h"

/* Kernel panic implementation.
   Intel SDM Vol.3 §8.2: On triple fault the CPU resets; panic prevents that.
   We dump all general-purpose registers, segment selectors, CR2 (fault addr),
   and spin forever with interrupts disabled.

   Re-entrancy safety:
   panic_active uses an atomic compare-exchange to prevent double-panic.
   Once set, any nested panic call spins immediately via panic_halt().
   This avoids corrupted output when a panic triggers another fault. */

static serial_dev_t *g_panic_serial;

/* Atomic re-entrancy guard — plain int is NOT safe for nested faults.
 * We use xchg (inherently locked) to atomically test-and-set. */
static volatile uint32_t panic_active;

void panic_init(void)
{
    /* Panic uses the global serial from main.c via panic_set_serial(),
     * or falls back to no output. We accept NULL gracefully. */
    g_panic_serial = 0;
    panic_active = 0;
}

void panic_set_serial(void *serial_dev)
{
    g_panic_serial = (serial_dev_t *)serial_dev;
}

static void pstr(const char *s) { print_str(g_panic_serial, s); }
static void phex(uint64_t v)    { print_hex64(g_panic_serial, v); }

static void print_reg(const char *name, uint64_t val)
{
    pstr(name);
    pstr("=0x");
    phex(val);
    pstr("  ");
}

void kernel_panic(const char *message, panic_regs_t *regs)
{
    /* Atomically set panic_active.  If already active, we are in a
     * nested panic — halt immediately to avoid infinite recursion. */
    uint32_t prev;
    asm volatile ("xchgl %0, %1"
                  : "=r"(prev), "+m"(panic_active)
                  : "r"(1)
                  : "memory", "cc");
    if (prev) {
        panic_halt();
    }

    /* Intel SDM Vol.3 §4.3: Clear interrupt flag to stop all scheduling. */
    asm volatile ("cli");

    pstr("\r\n");
    pstr("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\r\n");
    pstr("!!! KERNEL PANIC !!!\r\n");
    pstr("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\r\n\r\n");
    pstr("Message: ");
    pstr(message);
    pstr("\r\n\r\n");

    if (regs) {
        pstr("--- Register Dump ---\r\n");

        print_reg("RAX", regs->rax);
        print_reg("RBX", regs->rbx);
        print_reg("RCX", regs->rcx);
        print_reg("RDX", regs->rdx);
        pstr("\r\n");

        print_reg("RSI", regs->rsi);
        print_reg("RDI", regs->rdi);
        print_reg("RBP", regs->rbp);
        print_reg("RSP", regs->rsp);
        pstr("\r\n");

        print_reg("R8 ", regs->r8);
        print_reg("R9 ", regs->r9);
        print_reg("R10", regs->r10);
        print_reg("R11", regs->r11);
        pstr("\r\n");

        print_reg("R12", regs->r12);
        print_reg("R13", regs->r13);
        print_reg("R14", regs->r14);
        print_reg("R15", regs->r15);
        pstr("\r\n");

        print_reg("RIP", regs->rip);
        print_reg("RFL", regs->rflags);
        pstr("\r\n");

        print_reg("CS ", regs->cs);
        print_reg("SS ", regs->ss);
        print_reg("DS ", regs->ds);
        print_reg("ES ", regs->es);
        pstr("\r\n");

        print_reg("FS ", regs->fs);
        print_reg("GS ", regs->gs);
        print_reg("CR2", regs->cr2);
        pstr("\r\n");

        print_reg("ERR", regs->error_code);
        pstr("\r\n");
    }

    pstr("\r\n--- System Halted ---\r\n");
    panic_halt();
}

void panic_halt(void)
{
    asm volatile ("cli");
    for (;;) {
        asm volatile ("hlt");
    }
}
