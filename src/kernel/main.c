/**
 * @file main.c
 * @brief Kernel entry point — initializes subsystems and runs the test suite.
 *
 * Init order (rationale for each step):
 *   1. serial_init   — output channel for all subsequent logging and tests.
 *   2. isr_init      — zero the handler dispatch table. Must come before any
 *                       handler registration.
 *   3. gdt_init      — installs full GDT + TSS; safe to call before interrupts
 *                       because port I/O (serial) has no GDT dependency.
 *   4. pic_init      — remaps IRQ vectors 0-15 to INT 32-47 and masks all
 *                       IRQ lines. Must come before idt_init so the PIC is
 *                       quiescent before any interrupt gate is installed.
 *   5. idt_init      — builds 256 IDT gates, sets up IST1 for double-fault.
 *                       Must come after gdt_init (needs GDT CS selector) and
 *                       before any int instruction.
 *   6. register_fault_handlers — #GP (vec 13) and #PF (vec 14) diagnostic
 *                       dumps. Must come after idt_init and isr_init so
 *                       the IDT exists and handler slots are available.
 *   7. pmm_init      — parses the Multiboot2 memory map, builds the physical
 *                       frame bitmap, reserves kernel + bitmap frames.
 *                       Must come after fault handlers so #PF during map
 *                       parsing is caught rather than triple-faulting.
 *   8. test_run_all  — runs every registered test and prints pass/fail summary.
 */

#include "gdt.h"               /* lives in the same directory (src/kernel/) */
#include "idt.h"
#include "isr.h"
#include "pic.h"
#include "pmm.h"
#include "page_table.h"
#include "vmm.h"
#include "../drivers/serial.h"
#include "../tests/test.h"
#include <stdint.h>
#include <string.h>

/* ---- globals read by test_boot.c ---- */
uint32_t boot_magic    = 0;
uint32_t boot_info_ptr = 0;

/* ---- serial device shared with fault handlers ---- */
static serial_dev_t g_serial;

/* ---- test suite registration functions (defined in their own .c files) ---- */
extern void test_register_serial(void);
extern void test_register_boot(void);
extern void test_register_gdt(void);
extern void test_register_idt(void);
extern void test_register_pmm(void);
extern void test_register_vmm(void);

/* =========================================================================
 * Fault handlers — diagnostic dumps for PMM-critical exceptions
 * ========================================================================= */

/** Print a 64-bit value as a fixed-width hex string to serial. */
static void print_hex64(uint64_t val) {
    static const char hex[] = "0123456789ABCDEF";
    serial_write_string(&g_serial, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_write_char(&g_serial, hex[(val >> i) & 0xF]);
    }
}

/** Print a 32-bit value as a fixed-width hex string to serial. */
static void print_hex32(uint32_t val) {
    static const char hex[] = "0123456789ABCDEF";
    serial_write_string(&g_serial, "0x");
    for (int i = 28; i >= 0; i -= 4) {
        serial_write_char(&g_serial, hex[(val >> i) & 0xF]);
    }
}

/** Print a decimal unsigned integer to serial. */
static void print_uint64(uint64_t val) {
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (val == 0) {
        serial_write_char(&g_serial, '0');
        return;
    }
    while (val > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    serial_write_string(&g_serial, &buf[i]);
}

/**
 * @brief General Protection Fault handler (vector 13).
 *
 * Dumps the full interrupt frame to serial for debugging, then halts.
 * #GP fires on segment-selector errors, I/O port violations, and bad
 * linear addresses — all of which can be triggered by PMM bugs.
 */
static void handler_gp(interrupt_frame_t *frame) {
    serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "!!! #GP — General Protection Fault !!!\r\n");
    serial_write_string(&g_serial, "  Error code : "); print_hex64(frame->error_code); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  CS:RIP     = "); print_hex64(frame->cs); serial_write_string(&g_serial, ":");
    serial_write_string(&g_serial, ""); print_hex64(frame->rip); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  SS:RSP     = "); print_hex64(frame->ss); serial_write_string(&g_serial, ":");
    serial_write_string(&g_serial, ""); print_hex64(frame->rsp); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RFLAGS     = "); print_hex64(frame->rflags); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RAX = "); print_hex64(frame->rax); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RBX = "); print_hex64(frame->rbx); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RCX = "); print_hex64(frame->rcx); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RDX = "); print_hex64(frame->rdx); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RSI = "); print_hex64(frame->rsi); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RDI = "); print_hex64(frame->rdi); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RBP = "); print_hex64(frame->rbp); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R8  = "); print_hex64(frame->r8);  serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R9  = "); print_hex64(frame->r9);  serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R10 = "); print_hex64(frame->r10); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R11 = "); print_hex64(frame->r11); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R12 = "); print_hex64(frame->r12); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R13 = "); print_hex64(frame->r13); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R14 = "); print_hex64(frame->r14); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R15 = "); print_hex64(frame->r15); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  HALTING.\r\n");
    while (1) { __asm__ volatile ("cli; hlt"); }
}

/**
 * @brief Page Fault handler (vector 14).
 *
 * Attempts Copy-on-Write resolution first: if the fault is a write to a
 * COW-marked user page, allocates a private copy and returns (instruction
 * retries).  Otherwise dumps the full interrupt frame and halts.
 *
 * Reads CR2 to get the faulting linear address.
 *
 * Page fault error code bit fields (Intel SDM Vol.3A §6.15):
 *   bit 0: P   — 0 = fault caused by non-present page, 1 = protection violation
 *   bit 1: W/R — 0 = fault caused by a read,          1 = fault caused by a write
 *   bit 2: U/S — 0 = fault in supervisor mode,        1 = fault in user mode
 *   bit 3: RSVD — fault caused by reserved bit set in page directory entry
 *   bit 4: I/D — fault caused by instruction fetch
 */
static void handler_pf(interrupt_frame_t *frame) {
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    /* Attempt COW resolution: write to a COW-marked user page. */
    if ((frame->error_code & 2) && va_is_user(cr2) &&
        is_page_aligned(cr2)) {
        /* Walk the current page tables from CR3. */
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        uint64_t *pml4 = (uint64_t *)(uintptr_t)cr3; /* identity map */

        uint64_t pml4e = pml4[PML4_INDEX(cr2)];
        if (pml4e & PTE_PRESENT) {
            uint64_t *pdpt = (uint64_t *)(uintptr_t)pte_addr(pml4e);
            uint64_t pdpe = pdpt[PDPT_INDEX(cr2)];
            if ((pdpe & PTE_PRESENT) && !(pdpe & PTE_PS)) {
                uint64_t *pd = (uint64_t *)(uintptr_t)pte_addr(pdpe);
                uint64_t pde = pd[PD_INDEX(cr2)];
                if ((pde & PTE_PRESENT) && !(pde & PTE_PS)) {
                    uint64_t *pt = (uint64_t *)(uintptr_t)pte_addr(pde);
                    uint64_t *pte_ptr = &pt[PT_INDEX(cr2)];
                    uint64_t old_pte = *pte_ptr;

                    if (old_pte & PTE_COW) {
                        uint64_t new_frame = pmm_alloc_frame();
                        if (new_frame == 0) {
                            serial_write_string(&g_serial,
                                "COW: OOM — cannot allocate frame\r\n");
                            goto fault_halt;
                        }

                        /* Copy old page content to new frame. */
                        void *old_virt = (void *)(uintptr_t)pte_addr(old_pte);
                        void *new_virt = (void *)(uintptr_t)new_frame;
                        memcpy(new_virt, old_virt, PAGE_SIZE);

                        /* Remap: writable, COW cleared. */
                        uint64_t new_flags = pte_flags(old_pte)
                                           | PTE_WRITABLE;
                        new_flags &= ~PTE_COW;
                        *pte_ptr = pte_make(new_frame, new_flags);

                        pt_invlpg((void *)cr2);
                        return;  /* instruction retries */
                    }
                }
            }
        }
    }

fault_halt:
    serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "!!! #PF — Page Fault !!!\r\n");
    serial_write_string(&g_serial, "  Fault addr (CR2) = "); print_hex64(cr2); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  Error code        = "); print_hex64(frame->error_code); serial_write_string(&g_serial, "\r\n");

    /* Decode the error code into human-readable flags. */
    uint64_t err = frame->error_code;
    serial_write_string(&g_serial, "  Cause: ");
    if (!(err & 1)) serial_write_string(&g_serial, "non-present ");
    else            serial_write_string(&g_serial, "protection-violation ");
    if (err & 2) serial_write_string(&g_serial, "write ");
    else         serial_write_string(&g_serial, "read ");
    if (err & 4) serial_write_string(&g_serial, "user-mode ");
    serial_write_string(&g_serial, "\r\n");

    serial_write_string(&g_serial, "  CS:RIP = "); print_hex64(frame->cs); serial_write_string(&g_serial, ":");
    print_hex64(frame->rip); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  SS:RSP = "); print_hex64(frame->ss); serial_write_string(&g_serial, ":");
    print_hex64(frame->rsp); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RFLAGS = "); print_hex64(frame->rflags); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RAX = "); print_hex64(frame->rax); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RBX = "); print_hex64(frame->rbx); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RCX = "); print_hex64(frame->rcx); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RDX = "); print_hex64(frame->rdx); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RSI = "); print_hex64(frame->rsi); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RDI = "); print_hex64(frame->rdi); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  RBP = "); print_hex64(frame->rbp); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R8  = "); print_hex64(frame->r8);  serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R9  = "); print_hex64(frame->r9);  serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R10 = "); print_hex64(frame->r10); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R11 = "); print_hex64(frame->r11); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R12 = "); print_hex64(frame->r12); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R13 = "); print_hex64(frame->r13); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R14 = "); print_hex64(frame->r14); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  R15 = "); print_hex64(frame->r15); serial_write_string(&g_serial, "\r\n");
    serial_write_string(&g_serial, "  HALTING.\r\n");
    while (1) { __asm__ volatile ("cli; hlt"); }
}

/** Register page fault and #GP handlers for PMM-debug diagnostics. */
static void register_fault_handlers(void) {
    isr_register_handler(13, handler_gp);
    isr_register_handler(14, handler_pf);
}

void kernel_main(uint32_t magic, uint32_t info_ptr) {
    boot_magic    = magic;
    boot_info_ptr = info_ptr;

    /* --- 1. Bring up serial output first — everything else logs via it. --- */
    if (serial_init(&g_serial, SERIAL_COM1) != SERIAL_OK) {
        /* Serial unavailable: nothing we can do — spin forever. */
        while (1) { __asm__ volatile ("hlt"); }
    }

    serial_write_string(&g_serial, "\r\n======================================\r\n");
    serial_write_string(&g_serial, "       TsOs - Truly Simple OS          \r\n");
    serial_write_string(&g_serial, "======================================\r\n");
    serial_write_string(&g_serial, "[INFO] Kernel boot complete.\r\n");
    serial_write_string(&g_serial, "[INFO] Serial interface online.\r\n");

    /* --- 2. Initialize ISR handler table (zero all entries). --- */
    isr_init();
    serial_write_string(&g_serial, "[INFO] ISR handler table initialized.\r\n");

    /* --- 3. Install full GDT (6 descriptors) + TSS. --- */
    gdt_init();
    serial_write_string(&g_serial, "[INFO] GDT loaded (kernel/user code+data + TSS).\r\n");

    /* --- 4. Initialize 8259A PIC (remap IRQ vectors + mask all). --- */
    pic_init();
    serial_write_string(&g_serial, "[INFO] PIC initialized (IRQs remapped to INT 32-47).\r\n");

    /* --- 5. Install IDT (256 gates), set up IST1 for double-fault. --- */
    idt_init();
    serial_write_string(&g_serial, "[INFO] IDT loaded (256 vectors).\r\n");

    /* --- 6. Register diagnostic fault handlers (#GP, #PF). --- */
    register_fault_handlers();
    serial_write_string(&g_serial, "[INFO] Fault handlers registered (#GP, #PF).\r\n");

    /* --- 7. Initialize the Physical Memory Manager. --- */
    pmm_init(boot_info_ptr, &g_serial);
    serial_write_string(&g_serial, "[INFO] PMM initialized.\r\n");

    /* --- 7b. Initialize the Virtual Memory Manager. --- */
    vmm_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] VMM initialized.\r\n");

    /* --- 8. Register and run all tests. --- */
    test_register_serial();
    test_register_boot();
    test_register_gdt();
    test_register_idt();
    test_register_pmm();
    test_register_vmm();
    test_run_all(&g_serial);

    /* Test suite complete — idle forever. */
    while (1) {
        __asm__ volatile ("hlt");
    }
}

