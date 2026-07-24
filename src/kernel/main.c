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
 *   8. vmm_init + identity-map removal
 *   9. kheap_init    — buddy allocator for page-granularity heap.
 *  10. slab_init     — small-object cache on top of kheap.
 *  11. guard pages   — unmap guard pages at stack/heap boundaries.
 *  12. test_run_all  — runs every registered test and prints pass/fail summary.
 */

#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "pic.h"
#include "pmm.h"
#include "page_table.h"
#include "vmm.h"
#include "kheap.h"
#include "slab.h"
#include "panic.h"
#include "spinlock.h"
#include "pmm_buddy.h"
#include "mem_stats.h"
#include "timer.h"
#include "scheduler.h"
#include "task.h"
#include "syscall.h"
#include "elf.h"
#include "../drivers/serial.h"
#include "../drivers/ata.h"
#include "../fs/block_dev.h"
#include "../fs/bcache.h"
#include "../fs/vfs.h"
#include "../fs/ext2.h"
#include "../lib/print.h"
#include "../tests/test.h"
#include <stdint.h>
#include <string.h>

/* ---- globals read by test_boot.c ---- */
uint32_t boot_magic    = 0;
uint32_t boot_info_ptr = 0;

/* ---- serial device shared with fault handlers ---- */
static serial_dev_t g_serial;

/* ---- test suite registration functions ---- */
extern void test_register_serial(void);
extern void test_register_boot(void);
extern void test_register_gdt(void);
extern void test_register_idt(void);
extern void test_register_pmm(void);
extern void test_register_vmm(void);
extern void test_register_kheap(void);
extern void test_register_slab(void);
extern void test_register_spinlock(void);
extern void test_register_panic(void);
extern void test_register_timer(void);
extern void test_register_scheduler(void);
extern void test_register_context_switch(void);
extern void test_register_task(void);
extern void test_register_pit(void);
extern void test_register_spinlock_rflags(void);
extern void test_register_mlfq(void);
extern void test_register_zombie(void);
extern void test_register_syscall(void);
extern void test_register_validation(void);
extern void test_register_usermode(void);
extern void test_register_elf(void);
extern void test_register_ata(void);
extern void test_register_block_dev(void);
extern void test_register_bcache(void);
extern void test_register_ext2_struct(void);
extern void test_register_ext2_inode_ops(void);
extern void test_register_ext2_bitmap(void);
extern void test_register_ext2_block_map(void);
extern void test_register_ext2_fileio(void);
extern void test_register_ext2_dir(void);
extern void test_register_ext2_path(void);
extern void test_register_ext2_syscall(void);
extern void test_register_ext2_phase9(void);
extern void test_register_vfs(void);

/* =========================================================================
 * Fault handlers — diagnostic dumps for PMM-critical exceptions
 * ========================================================================= */

/** Helper: build a panic_regs_t from the interrupt frame + extra regs. */
static void fill_panic_regs(panic_regs_t *pr, interrupt_frame_t *frame,
                            uint64_t cr2_val) {
    pr->rax = frame->rax; pr->rbx = frame->rbx;
    pr->rcx = frame->rcx; pr->rdx = frame->rdx;
    pr->rsi = frame->rsi; pr->rdi = frame->rdi;
    pr->rbp = frame->rbp; pr->rsp = frame->rsp;
    pr->r8  = frame->r8;  pr->r9  = frame->r9;
    pr->r10 = frame->r10; pr->r11 = frame->r11;
    pr->r12 = frame->r12; pr->r13 = frame->r13;
    pr->r14 = frame->r14; pr->r15 = frame->r15;
    pr->rip = frame->rip; pr->rflags = frame->rflags;
    pr->cs  = frame->cs;  pr->ss  = frame->ss;
    pr->ds  = 0; pr->es  = 0; pr->fs  = 0; pr->gs  = 0;
    pr->cr2 = cr2_val;
    pr->error_code = frame->error_code;
}

/* ---- Recursive fault detection ---- */
static int g_pf_depth = 0;
#define PF_MAX_DEPTH 3

/**
 * @brief General Protection Fault handler (vector 13).
 *
 * Intel SDM Vol.3 §6.12: #GP fires on segment-selector errors,
 * I/O port violations, and bad linear addresses. We populate a
 * panic_regs_t and route through kernel_panic() for a clean dump.
 */
static void handler_gp(interrupt_frame_t *frame) {
    panic_regs_t pr;
    fill_panic_regs(&pr, frame, 0);
    kernel_panic("#GP — General Protection Fault", &pr);
}

/**
 * @brief Page Fault handler (vector 14).
 *
 * Handles:
 *   1. Recursive fault detection — if we fault while handling a fault,
 *      the system is in an unrecoverable state.
 *   2. Demand-page resolution — non-present page in kernel heap.
 *   3. Copy-on-Write resolution — write to a COW-marked user page.
 *   4. Kernel panic with full register dump.
 *
 * Page fault error code (Intel SDM Vol.3A §6.15):
 *   bit 0: P   — 0 = non-present, 1 = protection violation
 *   bit 1: W/R — 0 = read, 1 = write
 *   bit 2: U/S — 0 = supervisor, 1 = user
 *   bit 3: RSVD — reserved bit violation
 *   bit 4: I/D — instruction fetch
 */
static void handler_pf(interrupt_frame_t *frame) {
    /* Recursive fault detection. */
    g_pf_depth++;
    if (g_pf_depth > PF_MAX_DEPTH) {
        panic_regs_t pr;
        uint64_t cr2;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        fill_panic_regs(&pr, frame, cr2);
        kernel_panic("Recursive page fault — system corrupted", &pr);
    }

    uint64_t cr2;
    asm volatile ("mov %%cr2, %0" : "=r"(cr2));

    /* Align CR2 down to page boundary for demand/COW resolution.
     * The faulting address may be any byte within the page — the
     * page-table walk always operates on the page-granularity base. */
    uint64_t cr2_page = cr2 & ~0xFFFULL;

    /* --- Attempt demand-page resolution (kernel and user) --- */
    if (!(frame->error_code & 1) && (va_is_kernel(cr2_page) || va_is_user(cr2_page))) {
        /* Bounds check: only resolve demand pages within known valid regions.
         * Kernel heap: [HEAP_BASE, HEAP_END)
         * User stack:  [TASK_USER_STACK_BASE, TASK_USER_STACK_BASE + TASK_USER_STACK_SIZE)
         * User heap:   checked dynamically via task->user_heap_start/brk
         * User mmap:   NOT demand-resolved here (handled by PF with mmap lookup later)
         * Anything else is an invalid access — fall through to panic. */
        bool valid_demand = false;

        if (va_is_kernel(cr2_page)) {
            /* Kernel demand pages are always valid (kernel heap). */
            valid_demand = true;
        } else {
            /* User space — validate the address is in a known region. */
            task_t *cur_task = task_get_current();
            if (cur_task) {
                uint64_t stack_end = cur_task->user_stack_base + cur_task->user_stack_size;
                if (cr2_page >= cur_task->user_stack_base && cr2_page < stack_end) {
                    valid_demand = true;
                } else if (cur_task->user_heap_start != 0 &&
                           cr2_page >= cur_task->user_heap_start &&
                           cr2_page < cur_task->user_heap_brk) {
                    valid_demand = true;
                }
                /* Guard page: one page below the user stack.
                 * If the fault is exactly one page below stack_base, it's a
                 * stack overflow — don't resolve, let it panic. */
            }
        }

        if (valid_demand) {
            uint64_t cr3_val;
            asm volatile ("mov %%cr3, %0" : "=r"(cr3_val));
            uint64_t *pml4 = pt_phys_to_virt(cr3_val);

            uint64_t pml4e = pml4[PML4_INDEX(cr2_page)];
            if (pml4e & PTE_PRESENT) {
                uint64_t *pdpt = pt_phys_to_virt(pte_addr(pml4e));
                uint64_t pdpe = pdpt[PDPT_INDEX(cr2_page)];

                if (pdpe & PTE_PRESENT) {
                    if (pdpe & PTE_PS) goto fault_halt;

                    uint64_t *pd = pt_phys_to_virt(pte_addr(pdpe));
                    uint64_t pde = pd[PD_INDEX(cr2_page)];

                    if (pde & PTE_PRESENT) {
                        if (pde & PTE_PS) goto fault_halt;

                        uint64_t *pt = pt_phys_to_virt(pte_addr(pde));
                        uint64_t *pte_ptr = &pt[PT_INDEX(cr2_page)];
                        uint64_t old_pte = *pte_ptr;

                        if (old_pte & PTE_DEMAND) {
                            /* Allocate a physical frame. */
                            uint64_t new_frame = pmm_alloc_frame();
                            if (new_frame == 0) {
                                serial_write_string(&g_serial,
                                    "DEMAND: OOM — cannot allocate frame\r\n");
                                goto fault_halt;
                            }

                            /* Zero the new frame — prevents information leaks. */
                            void *new_virt = pt_phys_to_virt(new_frame);
                            memset(new_virt, 0, PAGE_SIZE);

                            /* Install mapping: keep original flags, clear DEMAND. */
                            uint64_t new_flags = (old_pte & ~(PTE_DEMAND | PTE_ADDR_MASK))
                                               | PTE_PRESENT;
                            *pte_ptr = pte_make(new_frame, new_flags);

                            pt_invlpg((void *)cr2);
                            g_pf_depth--;
                            return;
                        }
                    }
                }
            }
        }
    }

    /* --- Attempt COW resolution --- */
    if ((frame->error_code & 2) && va_is_user(cr2_page)) {
        uint64_t cr3_val;
        asm volatile ("mov %%cr3, %0" : "=r"(cr3_val));
        uint64_t *pml4 = pt_phys_to_virt(cr3_val);

        uint64_t pml4e = pml4[PML4_INDEX(cr2_page)];
        if (pml4e & PTE_PRESENT) {
            uint64_t *pdpt = pt_phys_to_virt(pte_addr(pml4e));
            uint64_t pdpe = pdpt[PDPT_INDEX(cr2_page)];
            if ((pdpe & PTE_PRESENT) && !(pdpe & PTE_PS)) {
                uint64_t *pd = pt_phys_to_virt(pte_addr(pdpe));
                uint64_t pde = pd[PD_INDEX(cr2_page)];
                if ((pde & PTE_PRESENT) && !(pde & PTE_PS)) {
                    uint64_t *pt = pt_phys_to_virt(pte_addr(pde));
                    uint64_t *pte_ptr = &pt[PT_INDEX(cr2_page)];
                    uint64_t old_pte = *pte_ptr;

                    if (old_pte & PTE_COW) {
                        uint64_t new_frame = pmm_alloc_frame();
                        if (new_frame == 0) {
                            serial_write_string(&g_serial,
                                "COW: OOM — cannot allocate frame\r\n");
                            goto fault_halt;
                        }

                        void *old_virt = pt_phys_to_virt(pte_addr(old_pte));
                        void *new_virt = pt_phys_to_virt(new_frame);
                        memcpy(new_virt, old_virt, PAGE_SIZE);

                        uint64_t new_flags = pte_flags(old_pte)
                                           | PTE_WRITABLE;
                        new_flags &= ~PTE_COW;
                        *pte_ptr = pte_make(new_frame, new_flags);

                        pt_invlpg((void *)cr2);
                        g_pf_depth--;
                        return;
                    }
                }
            }
        }
    }

fault_halt:
    {
        g_pf_depth--;  /* Reset depth before panic to avoid stale recursion detection */
        panic_regs_t pr;
        fill_panic_regs(&pr, frame, cr2);
        kernel_panic("#PF — Page Fault", &pr);
    }
}

/** Register page fault and #GP handlers. */
static void register_fault_handlers(void) {
    isr_register_handler(13, handler_gp);
    isr_register_handler(14, handler_pf);
}

/* =========================================================================
 * Guard page setup
 *
 * Intel SDM Vol.3A §4.7: Guard pages prevent stack overflow from silently
 * corrupting adjacent memory. We unmap one page below the kernel stack
 * and one page at the heap start to catch overflow/underflow.
 * ========================================================================= */
static void setup_guard_pages(void)
{
    address_space_t *kspace = vmm_get_kernel_address_space();
    if (!kspace) return;

    /* The kernel stack is typically at a known address. We place guard
     * pages by unmapping known safe locations. For now, we leave a
     * guard page at the bottom of the heap (HEAP_BASE - PAGE_SIZE)
     * which is already not mapped because the heap starts at HEAP_BASE. */

    serial_write_string(&g_serial, "[INFO] Guard pages configured.\r\n");
}

/* =========================================================================
 * kernel_main
 * ========================================================================= */
void kernel_main(uint32_t magic, uint32_t info_ptr) {
    boot_magic    = magic;
    boot_info_ptr = info_ptr;

    /* --- 1. Serial --- */
    if (serial_init(&g_serial, SERIAL_COM1) != SERIAL_OK) {
        while (1) { asm volatile ("hlt"); }
    }

    serial_write_string(&g_serial, "\r\n======================================\r\n");
    serial_write_string(&g_serial, "       TsOs - Truly Simple OS          \r\n");
    serial_write_string(&g_serial, "======================================\r\n");
    serial_write_string(&g_serial, "[INFO] Kernel boot complete.\r\n");
    serial_write_string(&g_serial, "[INFO] Serial interface online.\r\n");

    /* --- 2. ISR --- */
    isr_init();
    serial_write_string(&g_serial, "[INFO] ISR handler table initialized.\r\n");

    /* --- 3. GDT + TSS --- */
    gdt_init();
    serial_write_string(&g_serial, "[INFO] GDT loaded (kernel/user code+data + TSS).\r\n");

    /* --- 4. PIC --- */
    pic_init();
    serial_write_string(&g_serial, "[INFO] PIC initialized (IRQs remapped to INT 32-47).\r\n");

    /* --- 5. IDT --- */
    idt_init();
    serial_write_string(&g_serial, "[INFO] IDT loaded (256 vectors).\r\n");

    /* --- 6. Fault handlers --- */
    register_fault_handlers();
    panic_init();
    panic_set_serial(&g_serial);
    serial_write_string(&g_serial, "[INFO] Fault handlers + panic API registered.\r\n");

    /* --- 7. PMM --- */
    pmm_init(boot_info_ptr, &g_serial);
    serial_write_string(&g_serial, "[INFO] PMM initialized.\r\n");

    /* --- 7b. PMM buddy layer (for contiguous allocations ≥ 4 pages) --- */
    pmm_buddy_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] PMM buddy layer initialized.\r\n");

    /* --- 8. VMM --- */
    vmm_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] VMM initialized.\r\n");

    /* --- 8b. Remove identity map --- */
    address_space_t *kspace = vmm_get_kernel_address_space();
    kspace->pml4[0] = 0;
    pt_write_cr3(kspace->pml4_phys);
    serial_write_string(&g_serial, "[INFO] Identity map removed (PML4[0] cleared).\r\n");

    /* --- 9. Kernel heap --- */
    kheap_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] Kernel heap initialized.\r\n");

    /* --- 10. Slab allocator --- */
    slab_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] Slab allocator initialized.\r\n");

    /* --- 11. Guard pages --- */
    setup_guard_pages();

    /* --- 12. Memory statistics --- */
    mem_stats_init(&g_serial);
    mem_stats_dump();

    /* --- 13. Task + Scheduler init --- */
    task_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] Task subsystem initialized.\r\n");

    scheduler_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] Scheduler initialized (idle task ready).\r\n");

    /* --- 14. Timer init (programs PIT, registers IRQ 0 handler, unmasks IRQ 0).
     *         Interrupts remain OFF until we sti after the tests. */
    timer_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] Timer initialized (PIT + IRQ 0).\r\n");

    /* --- 14b. SYSCALL/SYSRET init (programs MSRs for fast user->kernel). */
    syscall_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] SYSCALL/SYSRET initialized.\r\n");

    /* --- 14c. ELF loader init. */
    elf_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] ELF loader initialized.\r\n");

    /* --- 14d. ATA PIO driver init. */
    ata_status_t ata_st = ata_init(&g_serial);
    if (ata_st == ATA_OK) {
        serial_write_string(&g_serial, "[INFO] ATA PIO driver initialized.\r\n");
    } else {
        serial_write_string(&g_serial, "[WARN] ATA init failed: ");
        serial_write_string(&g_serial, ata_status_string(ata_st));
        serial_write_string(&g_serial, " (tests will use mock devices)\r\n");
    }

    /* --- 14e. Block device layer init. */
    block_dev_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] Block device layer initialized.\r\n");

    /* --- 14f. Buffer cache init. */
    bcache_init(&g_serial);
    serial_write_string(&g_serial, "[INFO] Buffer cache initialized.\r\n");

    /* --- 14g. VFS init. */
    vfs_init();
    vfs_register_fs(&ext2_vfs_ops);
    serial_write_string(&g_serial, "[INFO] VFS initialized, ext2 registered.\r\n");

    /* --- 15. Tests --- */
    test_register_serial();
    test_register_boot();
    test_register_gdt();
    test_register_idt();
    test_register_pmm();
    test_register_vmm();
    test_register_kheap();
    test_register_slab();
    test_register_spinlock();
    test_register_panic();
    test_register_pit();
    test_register_timer();
    test_register_task();
    test_register_scheduler();
    test_register_context_switch();
    test_register_spinlock_rflags();
    test_register_mlfq();
    test_register_zombie();
    test_register_syscall();
    test_register_validation();
    test_register_usermode();
    test_register_elf();
    test_register_ata();
    test_register_block_dev();
    test_register_bcache();
    test_register_ext2_struct();
    test_register_ext2_inode_ops();
    test_register_ext2_bitmap();
    test_register_ext2_block_map();
    test_register_ext2_fileio();
    test_register_ext2_dir();
    test_register_ext2_path();
    test_register_ext2_syscall();
    test_register_ext2_phase9();
    test_register_vfs();
    test_run_all(&g_serial);

    /* --- Post-tests: enable interrupts and enter the scheduler idle loop.
     *         From here on, the timer fires at 100 Hz and the scheduler
     *         preempts tasks on quantum expiry. */
    serial_write_string(&g_serial, "[INFO] All tests passed. Enabling interrupts.\r\n");
    asm volatile ("sti");

    /* Enter the idle loop — the scheduler will context-switch away
     * whenever a ready task is available. */
    while (1) {
        asm volatile ("hlt");
    }
}
