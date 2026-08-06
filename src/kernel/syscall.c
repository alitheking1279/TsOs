/**
 * @file syscall.c
 * @brief SYSCALL/SYSRET implementation — MSR setup, dispatch, validation.
 */

#include "syscall.h"
#include "gdt.h"
#include "task.h"
#include "scheduler.h"
#include "page_table.h"
#include "vmm.h"
#include "slab.h"
#include "elf.h"
#include "../drivers/serial.h"
#include "../drivers/pit.h"
#include "../drivers/ps2.h"
#include "../drivers/vga.h"
#include "../drivers/pcspk.h"
#include "../drivers/portio.h"
#include "../fs/ext2.h"
#include "../fs/vfs.h"
#include "../fs/bcache.h"
#include "../kernel/kheap.h"
#include "../kernel/timer.h"
#include "pmm.h"
#include <sys/stat.h>
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Per-CPU data (single-core: one static instance)
 * ========================================================================= */

static per_cpu_data_t g_per_cpu __attribute__((aligned(16)));

/** Serial device for syscall logging. */
static serial_dev_t *g_sc_serial = NULL;

/** Address of the syscall_entry trampoline (from syscall_entry.asm). */
extern void syscall_entry(void);
uint64_t syscall_entry_addr;

/* =========================================================================
 * Per-CPU RSP management
 * ========================================================================= */

void syscall_set_kernel_rsp(uint64_t rsp) {
    g_per_cpu.kernel_rsp = rsp;
}

/* =========================================================================
 * Logging helper
 * ========================================================================= */

static void sc_log(const char *msg) {
    if (g_sc_serial) serial_write_string(g_sc_serial, msg);
}

/* =========================================================================
 * MSR initialization
 * ========================================================================= */

void syscall_init(void *serial_dev) {
    g_sc_serial = (serial_dev_t *)serial_dev;
    syscall_entry_addr = (uint64_t)&syscall_entry;

    sc_log("[SYSCALL] Initializing SYSCALL/SYSRET...\r\n");

    /* Set KERNEL_GS_BASE to point to per_cpu_data so syscall_entry can
     * swapgs and read the kernel stack pointer via GS:[0].  SWAPGS swaps
     * MSR_GS_BASE with MSR_KERNEL_GS_BASE — the kernel base must live in
     * KERNEL_GS_BASE, and user-mode GS must be zero/controlled. */
    uint64_t gs_base = (uint64_t)&g_per_cpu;
    write_msr(MSR_KERNEL_GS_BASE, gs_base);

    /* STAR register:
     *   bits [63:48] = SYSRET selector base.  The CPU derives the user
     *                  selectors from this base: SYSRET loads CS = base+16
     *                  and SS = base+8 (Intel SDM Vol.3A §3.3.1).  To land
     *                  on the user segments the data descriptor must sit one
     *                  GDT index below the code descriptor (see gdt.h), so
     *                  base = GDT_USER_CS_SEL - 16 = 0x13:
     *                    SYSRET SS = 0x1B (GDT[3] user data), CS = 0x23 (GDT[4])
     *   bits [47:32] = SYSCALL CS selector (0x08 = GDT[1]); the CPU derives
     *                  SYSCALL SS as CS+8 = 0x10 (GDT[2] kernel data).
     *   bits [31:0]  = reserved (must be zero).
     */
    uint64_t star_val = 0;
    star_val |= (uint64_t)(GDT_USER_CS_SEL - 16) << 48;
    star_val |= (uint64_t)GDT_KERNEL_CS_SEL << 32;
    write_msr(MSR_STAR, star_val);

    /* LSTAR = address of syscall_entry trampoline. */
    write_msr(MSR_LSTAR, (uint64_t)&syscall_entry);

    /* SFMASK = 0x200 (IF flag): disable interrupts on SYSCALL entry. */
    write_msr(MSR_SFMASK, 0x200);

    sc_log("[SYSCALL] MSRs programmed: STAR, LSTAR, SFMASK, GS_BASE.\r\n");
    sc_log("[SYSCALL] Init complete.\r\n");
}

/* =========================================================================
 * Input validation — page table walk
 * ========================================================================= */

/**
 * @brief Walk the page tables and return the leaf PTE flags for a vaddr.
 * Returns 0 if any level is not present.
 */
static uint64_t get_pte_flags(uint64_t vaddr) {
    task_t *cur = task_get_current();
    if (!cur || !cur->address_space) return 0;
    address_space_t *as = cur->address_space;
    uint64_t *pml4 = as->pml4;

    uint64_t pml4e = pml4[PML4_INDEX(vaddr)];
    if (!(pml4e & PTE_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t *)pt_phys_to_virt(pte_addr(pml4e));
    uint64_t pdpe = pdpt[PDPT_INDEX(vaddr)];
    if (!(pdpe & PTE_PRESENT)) return 0;
    if (pdpe & PTE_PS) return pdpe;

    uint64_t *pd = (uint64_t *)pt_phys_to_virt(pte_addr(pdpe));
    uint64_t pde = pd[PD_INDEX(vaddr)];
    if (!(pde & PTE_PRESENT)) return 0;
    if (pde & PTE_PS) return pde;

    uint64_t *pt = (uint64_t *)pt_phys_to_virt(pte_addr(pde));
    return pt[PT_INDEX(vaddr)];
}

int validate_user_pointer(const void *ptr, size_t size) {
    uint64_t start = (uint64_t)ptr;

    /* Must be in user half (lower canonical range). */
    if (start < 0x1000) return 0;
    if (size == 0) return 0;

    /* Overflow check: if start + size wraps around, it's invalid. */
    if (start + size < start) return 0;
    uint64_t end = start + size;
    if (end > 0x00007FFFFFFFFFFFULL) return 0;

    /* Check every page in the range. */
    for (uint64_t addr = start & ~((uint64_t)PAGE_SIZE - 1);
         addr < end;
         addr += PAGE_SIZE) {
        uint64_t pte = get_pte_flags(addr);
        if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) {
            return 0;
        }
    }
    return 1;
}

int validate_user_string(const char *str, size_t max_len) {
    uint64_t addr = (uint64_t)str;
    if (addr < 0x1000) return -1;

    for (size_t i = 0; i < max_len; i++) {
        /* Check page boundary crossings. */
        uint64_t page = (addr + i) & ~((uint64_t)PAGE_SIZE - 1);
        if (i == 0 || page != ((addr + i - 1) & ~((uint64_t)PAGE_SIZE - 1))) {
            uint64_t pte = get_pte_flags(page);
            if (!(pte & PTE_PRESENT)) return -1;
            if (!(pte & PTE_USER))    return -1;
        }
        char c = *(volatile char *)(addr + i);
        if (c == 0) return (int)i;
    }
    return -1;
}

/* =========================================================================
 * Syscall frame offsets
 *
 * The assembly trampoline (syscall_entry.asm) builds this frame layout
 * on the kernel stack. Offsets are from the frame pointer passed to
 * syscall_handler (which points to the saved rax slot).
 *
 *   Offset  Register saved by trampoline
 *   +0      rax
 *   +8      r10
 *   +16     r9
 *   +24     r8
 *   +32     rdx
 *   +40     rsi
 *   +48     rdi
 *   +56     rbx
 *   +64     rbp
 *   +72     r12
 *   +80     r13
 *   +88     r14
 *   +96     r15
 *   +104    r11  (saved separately, not in struct)
 *   +112    rcx  (saved separately, not in struct)
 * ========================================================================= */

#define SC_OFF_RAX   0
#define SC_OFF_R10   8
#define SC_OFF_R9    16
#define SC_OFF_R8    24
#define SC_OFF_RDX   32
#define SC_OFF_RSI   40
#define SC_OFF_RDI   48
#define SC_OFF_RCX   112  /* user RIP (saved by syscall_entry.asm) */
#define SC_OFF_R11   104  /* user RFLAGS (saved by syscall_entry.asm) */

/* =========================================================================
 * Syscall handlers
 * ========================================================================= */

static int64_t sys_write(uint64_t *frame) {
    int fd          = (int)frame[SC_OFF_RDI / 8];
    const char *buf = (const char *)frame[SC_OFF_RSI / 8];
    size_t count    = (size_t)frame[SC_OFF_RDX / 8];

    /* fd 1 = serial (stdout). */
    if (fd == 1) {
        if (!validate_user_pointer(buf, count)) return -1;
        serial_dev_t *s = g_sc_serial;
        if (!s) return -1;
        for (size_t i = 0; i < count; i++) {
            serial_write_char(s, buf[i]);
        }
        return (int64_t)count;
    }

    /* fd >= 2 = ext2 file. */
    task_t *cur = task_get_current();
    if (!cur) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS) return -1;
    if (!cur->fd_table[fd].ops || !cur->fd_table[fd].ops->write) return -1;
    if (!validate_user_pointer(buf, count)) return -1;
    return cur->fd_table[fd].ops->write(fd, buf, count, cur->fd_table[fd].data);
}

static int64_t sys_read(uint64_t *frame) {
    int fd       = (int)frame[SC_OFF_RDI / 8];
    void *buf    = (void *)frame[SC_OFF_RSI / 8];
    size_t count = (size_t)frame[SC_OFF_RDX / 8];

    /* fd 1 = serial out (not readable, return error). */
    if (fd == 1) return -1;

    /* fd >= 2 = ext2 file. */
    task_t *cur = task_get_current();
    if (!cur) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS) return -1;
    if (!cur->fd_table[fd].ops || !cur->fd_table[fd].ops->read) return -1;
    if (!validate_user_pointer(buf, count)) return -1;
    return cur->fd_table[fd].ops->read(fd, buf, count, cur->fd_table[fd].data);
}

static int64_t sys_exit(uint64_t *frame) {
    int code = (int)frame[SC_OFF_RDI / 8];
    task_exit(code);
    while (1) { asm volatile ("hlt"); }
    return 0;
}

static int64_t sys_fork(uint64_t *frame) {
    task_t *parent = task_get_current();
    if (!parent) return -1;

    task_t *child = task_create("child", NULL, false);
    if (!child) return -1;

    /* Set parent pointer. */
    child->parent = parent;
    child->sibling_next = parent->children;
    parent->children = child;

    /* Clone address space. */
    vmm_status_t st = vmm_clone_address_space(parent->address_space,
                                              child->address_space);
    if (st != VMM_OK) {
        task_destroy(child);
        return -1;
    }

    /* Extract parent's user RIP and RFLAGS from the syscall frame.
     * RCX at offset +112 holds the user RIP (SYSCALL saves RIP → RCX).
     * R11 at offset +104 holds the user RFLAGS (SYSCALL saves RFLAGS → R11). */
    uint64_t user_rip = frame[SC_OFF_RCX / 8];
    uint64_t user_rflags = frame[SC_OFF_R11 / 8];

    /* Duplicate parent's file descriptor table into child. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (parent->fd_table[i].ops) {
            child->fd_table[i] = parent->fd_table[i];
            child->fd_table[i].refcount++;
        }
    }

    /* Build child's kernel stack: context_switch frame → iretq_trampoline
     * → IRETQ frame with parent's user RIP, RFLAGS, and RSP. */
    extern void user_task_iretq_trampoline(void);
    uint64_t stack_top = child->kernel_stack_base + child->kernel_stack_size;
    stack_top &= ~0xFULL;

    uint64_t *sp = (uint64_t *)stack_top;
    sp--; *sp = (uint64_t)&user_task_iretq_trampoline;  /* ret target */
    sp--; *sp = 0;   /* r15 */
    sp--; *sp = 0;   /* r14 */
    sp--; *sp = 0;   /* r13 */
    sp--; *sp = 0;   /* r12 */
    sp--; *sp = 0;   /* rbp */
    sp--; *sp = 0;   /* rbx */
    child->kernel_rsp = (uint64_t)sp;

    /* Build the IRETQ frame — child resumes at parent's user RIP. */
    uint64_t user_stack_top = child->user_stack_base + child->user_stack_size;
    sp--; *sp = user_rip;              /* RIP = parent's user RIP */
    sp--; *sp = GDT_USER_CS_SEL;      /* CS */
    sp--; *sp = user_rflags;           /* RFLAGS = parent's RFLAGS */
    sp--; *sp = user_stack_top;        /* RSP */
    sp--; *sp = GDT_USER_DS_SEL;      /* SS */

    scheduler_add_task(child);

    /* Return child PID to parent (parent gets child's PID). */
    return (int64_t)child->pid;
}

static int64_t sys_getpid(uint64_t *frame) {
    (void)frame;
    task_t *cur = task_get_current();
    if (!cur) return -1;
    return (int64_t)cur->pid;
}

static int64_t sys_getppid(uint64_t *frame) {
    (void)frame;
    task_t *cur = task_get_current();
    if (!cur || !cur->parent) return 1;
    return (int64_t)cur->parent->pid;
}

static int64_t sys_wait(uint64_t *frame) {
    int *status_ptr = (int *)frame[SC_OFF_RDI / 8];
    if (status_ptr) {
        if (!validate_user_pointer(status_ptr, sizeof(int))) return -1;
    }

    int status = 0;
    int child_pid = task_wait(&status);

    if (status_ptr && child_pid >= 0) {
        *status_ptr = status;
    }
    return (int64_t)child_pid;
}

/* =========================================================================
 * brk — Heap Management
 * ========================================================================= */

static int64_t sys_brk(uint64_t *frame) {
    uint64_t new_brk = frame[SC_OFF_RDI / 8];
    task_t *cur = task_get_current();
    if (!cur) return -1;

    /* If new_brk is 0, return current break (query). */
    if (new_brk == 0) {
        return (int64_t)cur->user_heap_brk;
    }

    /* Validate new_brk is in user half. */
    if (new_brk >= 0x800000000000ULL) return -1;

    /* new_brk must be at or above heap_start. */
    if (cur->user_heap_start == 0) {
        /* Heap not yet initialized — set start to new_brk. */
        cur->user_heap_start = new_brk & ~(PAGE_SIZE - 1);
        cur->user_heap_brk = new_brk;
        return (int64_t)cur->user_heap_brk;
    }

    if (new_brk < cur->user_heap_start) return -1;

    uint64_t old_brk = cur->user_heap_brk;
    uint64_t new_brk_page = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t old_brk_page = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Grow: map demand pages for new pages. */
    if (new_brk_page > old_brk_page) {
        uint64_t heap_flags = VMM_FLAG_WRITE | VMM_FLAG_USER;
        for (uint64_t va = old_brk_page; va < new_brk_page; va += PAGE_SIZE) {
            vmm_status_t st = vmm_map_demand_page(cur->address_space, va,
                                                   heap_flags);
            if (st != VMM_OK && st != VMM_ERR_ALREADY_MAP) return -1;
        }
    }
    /* Shrink: unmap pages above new_brk. */
    else if (new_brk_page < old_brk_page) {
        for (uint64_t va = old_brk_page - PAGE_SIZE;
             va >= new_brk_page; va -= PAGE_SIZE) {
            vmm_unmap_and_free(cur->address_space, va);
        }
    }

    cur->user_heap_brk = new_brk;
    return (int64_t)cur->user_heap_brk;
}

/* =========================================================================
 * mmap — Memory Mapping (simplified)
 * ========================================================================= */

static int64_t sys_mmap(uint64_t *frame) {
    uint64_t addr = frame[SC_OFF_RDI / 8];    /* hint address (0 = let OS choose) */
    uint64_t length = frame[SC_OFF_RSI / 8];  /* length in bytes */
    uint64_t prot = frame[SC_OFF_RDX / 8];    /* protection flags */
    uint64_t flags = frame[SC_OFF_R10 / 8];   /* mmap flags (ignored for now) */
    (void)flags;

    task_t *cur = task_get_current();
    if (!cur) return -1;

    /* Validate inputs. */
    if (length == 0 || length > ELF_MAX_LOAD_SIZE) return -1;

    /* Round up to page boundary. */
    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t total = pages * PAGE_SIZE;

    /* Choose a free address if hint is 0 or below heap.
     * For simplicity, start mmap region after heap + 16 MiB. */
    uint64_t mmap_base = cur->user_heap_brk + (16 * 1024 * 1024);
    mmap_base = (mmap_base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (addr != 0 && addr >= cur->user_heap_start) {
        mmap_base = addr & ~(PAGE_SIZE - 1);
    }

    /* Convert mmap prot to PTE flags. */
    uint64_t pte_flags = PTE_USER;
    if (prot & 0x1) pte_flags |= PTE_PRESENT;  /* PROT_READ */
    if (prot & 0x2) pte_flags |= PTE_WRITABLE;  /* PROT_WRITE */
    if (!(prot & 0x4)) pte_flags |= PTE_NX;     /* no PROT_EXEC → NX */

    /* Map demand pages. */
    for (uint64_t va = mmap_base; va < mmap_base + total; va += PAGE_SIZE) {
        vmm_status_t st = vmm_map_demand_page(cur->address_space, va, pte_flags);
        if (st != VMM_OK && st != VMM_ERR_ALREADY_MAP) return -1;
    }

    return (int64_t)mmap_base;
}

/* =========================================================================
 * munmap — Unmap Memory (simplified)
 * ========================================================================= */

static int64_t sys_munmap(uint64_t *frame) {
    uint64_t addr = frame[SC_OFF_RDI / 8];
    uint64_t length = frame[SC_OFF_RSI / 8];

    task_t *cur = task_get_current();
    if (!cur) return -1;

    if (addr == 0 || length == 0) return -1;

    /* Must be page-aligned. */
    addr &= ~(PAGE_SIZE - 1);
    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t total = pages * PAGE_SIZE;

    /* Unmap each page using vmm_unmap_page — handles both present and
     * demand PTEs, frees physical frames, and cleans up empty page tables. */
    for (uint64_t va = addr; va < addr + total; va += PAGE_SIZE) {
        vmm_unmap_page(cur->address_space, va);
    }

    return 0;
}

/* =========================================================================
 * execve — Execute ELF Binary
 * ========================================================================= */

static int64_t sys_execve(uint64_t *frame) {
    uint64_t elf_addr = frame[SC_OFF_RDI / 8];   /* pointer to ELF data */
    uint64_t elf_size = frame[SC_OFF_RSI / 8];   /* size of ELF data */

    task_t *cur = task_get_current();
    if (!cur) return -1;

    /* Validate the ELF data pointer is in user space. */
    if (!validate_user_pointer((const void *)elf_addr, elf_size)) return -1;

    /* Create a temporary address space for the new ELF.
     * This avoids destroying the old address space before we know the load
     * succeeded — prevents leaving the task with an empty address space on
     * ELF validation failure. */
    address_space_t *new_as = (address_space_t *)slab_alloc(sizeof(address_space_t));
    if (!new_as) return -1;

    vmm_status_t st = vmm_create_address_space(new_as);
    if (st != VMM_OK) {
        slab_free(new_as);
        return -1;
    }

    /* Load the ELF into the new address space. */
    elf_load_result_t result;
    elf_status_t elf_st = elf_load((const void *)elf_addr, elf_size,
                                    new_as, &result);
    if (elf_st != ELF_OK) {
        /* ELF load failed — destroy the new address space, keep the old one. */
        vmm_destroy_address_space(new_as);
        slab_free(new_as);
        return -1;
    }

    /* ELF load succeeded — now destroy the old address space. */
    if (cur->address_space) {
        vmm_destroy_address_space(cur->address_space);
        slab_free(cur->address_space);
    }
    cur->address_space = new_as;

    /* Set up user stack (demand-paged). */
    cur->user_stack_base = TASK_USER_STACK_BASE;
    cur->user_stack_size = TASK_USER_STACK_SIZE;
    uint64_t stack_flags = VMM_FLAG_WRITE | VMM_FLAG_USER;
    for (uint64_t va = cur->user_stack_base;
         va < cur->user_stack_base + cur->user_stack_size;
         va += PAGE_SIZE) {
        vmm_map_demand_page(cur->address_space, va, stack_flags);
    }

    /* Set up heap after the highest loaded segment. */
    cur->user_heap_start = (result.top + PAGE_SIZE - 1)
                         & ~(PAGE_SIZE - 1);
    cur->user_heap_brk = cur->user_heap_start;

    /* Set up new kernel stack frame for iretq to user mode. */
    extern void user_task_iretq_trampoline(void);
    uint64_t stack_top = cur->kernel_stack_base + cur->kernel_stack_size;
    stack_top &= ~0xFULL;

    uint64_t *sp = (uint64_t *)stack_top;
    sp--; *sp = (uint64_t)&user_task_iretq_trampoline;  /* ret target */
    sp--; *sp = 0;   /* r15 */
    sp--; *sp = 0;   /* r14 */
    sp--; *sp = 0;   /* r13 */
    sp--; *sp = 0;   /* r12 */
    sp--; *sp = 0;   /* rbp */
    sp--; *sp = 0;   /* rbx */

    /* IRETQ frame below context_switch frame. */
    uint64_t user_stack_top = cur->user_stack_base + cur->user_stack_size;
    sp--; *sp = result.entry;            /* RIP = ELF entry */
    sp--; *sp = GDT_USER_CS_SEL;        /* CS */
    sp--; *sp = 0x202;                   /* RFLAGS (IF=1) */
    sp--; *sp = user_stack_top;          /* RSP */
    sp--; *sp = GDT_USER_DS_SEL;        /* SS */

    cur->kernel_rsp = (uint64_t)sp;

    /* Sync per-CPU kernel_rsp so the next SYSCALL uses the correct stack. */
    syscall_set_kernel_rsp((uint64_t)sp);

    return 0;
}

/* =========================================================================
 * open / close — VFS file descriptor operations
 * ========================================================================= */

static int64_t sys_open(uint64_t *frame) {
    const char *user_path = (const char *)frame[SC_OFF_RDI / 8];
    uint64_t flags        = frame[SC_OFF_RSI / 8];

    /* Validate full user string before kernel copy (prevents #PF on unmapped pages). */
    int path_len = validate_user_string(user_path, EXT2_MAX_PATH);
    if (path_len < 0) return -1;

    /* Copy path to kernel buffer. */
    char kpath[EXT2_MAX_PATH];
    const char *src = user_path;
    int i = 0;
    while (src[i] && i < EXT2_MAX_PATH - 1) { kpath[i] = src[i]; i++; }
    kpath[i] = '\0';

    return (int64_t)vfs_open(kpath, (uint32_t)flags);
}

static int64_t sys_close(uint64_t *frame) {
    int fd = (int)frame[SC_OFF_RDI / 8];

    task_t *cur = task_get_current();
    if (!cur) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS) return -1;
    if (!cur->fd_table[fd].ops) return -1;

    if (cur->fd_table[fd].ops->close)
        cur->fd_table[fd].ops->close(fd, cur->fd_table[fd].data);

    memset(&cur->fd_table[fd], 0, sizeof(file_t));
    return 0;
}

/* =========================================================================
 * fstat / lseek / unlink / getdents / rename (Phase 9)
 * ========================================================================= */

static int64_t sys_fstat(uint64_t *frame) {
    int fd = (int)frame[SC_OFF_RDI / 8];
    void *user_buf = (void *)frame[SC_OFF_RSI / 8];

    if (!validate_user_pointer(user_buf, sizeof(struct stat))) return -1;

    task_t *cur = task_get_current();
    if (!cur || fd < 0 || fd >= TASK_MAX_FDS) return -1;
    if (!cur->fd_table[fd].ops) return -1;

    /* We need the inode number from the ext2_file_t data. */
    ext2_file_t *file = (ext2_file_t *)cur->fd_table[fd].data;
    if (!file) return -1;

    ext2_stat_t st;
    if (ext2_stat(file->ino, &st) != EXT2_OK) return -1;

    /* Translate the native ext2_stat_t into the POSIX struct stat layout
     * used by userspace (src/libc/include/sys/stat.h). */
    struct stat out;
    memset(&out, 0, sizeof(out));
    out.st_dev     = 0;
    out.st_ino     = st.ino;
    out.st_mode    = st.mode;
    out.st_nlink   = st.links;
    out.st_uid     = st.uid;
    out.st_gid     = st.gid;
    out.st_rdev    = 0;
    out.st_size    = st.size;
    out.st_blksize = 512;
    out.st_blocks  = st.blocks;
    out.st_atime   = st.atime;
    out.st_mtime   = st.mtime;
    out.st_ctime   = st.ctime;

    memcpy(user_buf, &out, sizeof(out));
    return 0;
}

static int64_t sys_lseek(uint64_t *frame) {
    int fd = (int)frame[SC_OFF_RDI / 8];
    int64_t offset = (int64_t)frame[SC_OFF_RSI / 8];
    int whence = (int)frame[SC_OFF_RDX / 8];

    task_t *cur = task_get_current();
    if (!cur || fd < 0 || fd >= TASK_MAX_FDS) return -1;
    if (!cur->fd_table[fd].ops) return -1;

    ext2_file_t *file = (ext2_file_t *)cur->fd_table[fd].data;
    if (!file) return -1;

    ext2_stat_t st;
    if (ext2_stat(file->ino, &st) != EXT2_OK) return -1;

    int64_t new_pos;
    switch (whence) {
        case EXT2_SEEK_SET: new_pos = offset; break;
        case EXT2_SEEK_CUR: new_pos = (int64_t)file->offset + offset; break;
        case EXT2_SEEK_END: new_pos = (int64_t)st.size + offset; break;
        default: return -1;
    }

    if (new_pos < 0) new_pos = 0;
    file->offset = (uint64_t)new_pos;
    return new_pos;
}

static int64_t sys_unlink(uint64_t *frame) {
    const char *user_path = (const char *)frame[SC_OFF_RDI / 8];
    if (!validate_user_string(user_path, EXT2_MAX_PATH)) return -1;

    char kpath[EXT2_MAX_PATH];
    const char *src = user_path;
    int i = 0;
    while (src[i] && i < EXT2_MAX_PATH - 1) { kpath[i] = src[i]; i++; }
    kpath[i] = '\0';

    return (int64_t)vfs_unlink(kpath);
}

static int64_t sys_getdents(uint64_t *frame) {
    int fd = (int)frame[SC_OFF_RDI / 8];
    void *user_buf = (void *)frame[SC_OFF_RSI / 8];
    uint32_t count = (uint32_t)frame[SC_OFF_RDX / 8];

    if (!validate_user_pointer(user_buf, count)) return -1;

    task_t *cur = task_get_current();
    if (!cur || fd < 0 || fd >= TASK_MAX_FDS) return -1;
    if (!cur->fd_table[fd].ops) return -1;

    ext2_file_t *file = (ext2_file_t *)cur->fd_table[fd].data;
    if (!file) return -1;

    int32_t result = ext2_getdents(file->ino, &file->offset, user_buf, count);
    return (int64_t)result;
}

static int64_t sys_rename(uint64_t *frame) {
    const char *user_old = (const char *)frame[SC_OFF_RDI / 8];
    const char *user_new = (const char *)frame[SC_OFF_RSI / 8];

    if (!validate_user_string(user_old, EXT2_MAX_PATH)) return -1;
    if (!validate_user_string(user_new, EXT2_MAX_PATH)) return -1;

    char old_path[EXT2_MAX_PATH];
    const char *src = user_old;
    int i = 0;
    while (src[i] && i < EXT2_MAX_PATH - 1) { old_path[i] = src[i]; i++; }
    old_path[i] = '\0';

    char new_path[EXT2_MAX_PATH];
    src = user_new;
    i = 0;
    while (src[i] && i < EXT2_MAX_PATH - 1) { new_path[i] = src[i]; i++; }
    new_path[i] = '\0';

    return (int64_t)vfs_rename(old_path, new_path);
}

/* =========================================================================
 * usleep — busy-wait for given microseconds
 * ========================================================================= */

static int64_t sys_usleep(uint64_t *frame) {
    uint64_t usec = frame[SC_OFF_RDI / 8];
    if (usec == 0) return 0;

    /* Convert microseconds to ticks (1 tick = 10 ms = 10000 usec).
     * Always wait at least 1 tick for non-zero requests. */
    uint64_t start = timer_get_ticks();
    uint64_t ticks_per_sec = 100;  /* PIT_DEFAULT_FREQ */
    uint64_t target_ticks = (usec * ticks_per_sec) / 1000000;
    if (target_ticks == 0) target_ticks = 1;

    while ((timer_get_ticks() - start) < target_ticks) {
        __asm__ volatile ("hlt");
    }
    return 0;
}

/* =========================================================================
 * gettimeofday — convert monotonic ticks to struct timeval
 * ========================================================================= */

static int64_t sys_gettimeofday(uint64_t *frame) {
    uint64_t tv_addr = frame[SC_OFF_RDI / 8];

    if (tv_addr != 0) {
        if (!validate_user_pointer((void *)tv_addr, 16)) return -1;

        /* PIT runs at 100 Hz → 1 tick = 10 ms = 10000 usec. */
        uint64_t ticks = timer_get_ticks();
        long sec  = (long)(ticks / 100);
        long usec = (long)((ticks % 100) * 10000);

        long *tv = (long *)tv_addr;
        tv[0] = sec;
        tv[1] = usec;
    }
    return 0;
}

/* =========================================================================
 * mkdir / rmdir — Directory creation/removal (stubs for shell support)
 * ========================================================================= */

static int64_t sys_mkdir(uint64_t *frame) {
    const char *user_path = (const char *)frame[SC_OFF_RDI / 8];
    uint64_t mode         = frame[SC_OFF_RSI / 8];
    if (!validate_user_string(user_path, EXT2_MAX_PATH)) return -1;

    char kpath[EXT2_MAX_PATH];
    const char *src = user_path;
    int i = 0;
    while (src[i] && i < EXT2_MAX_PATH - 1) { kpath[i] = src[i]; i++; }
    kpath[i] = '\0';

    return (int64_t)vfs_mkdir(kpath, (uint32_t)mode);
}

static int64_t sys_rmdir(uint64_t *frame) {
    const char *user_path = (const char *)frame[SC_OFF_RDI / 8];
    if (!validate_user_string(user_path, EXT2_MAX_PATH)) return -1;

    char kpath[EXT2_MAX_PATH];
    const char *src = user_path;
    int i = 0;
    while (src[i] && i < EXT2_MAX_PATH - 1) { kpath[i] = src[i]; i++; }
    kpath[i] = '\0';

    return (int64_t)vfs_rmdir(kpath);
}

/* =========================================================================
 * get_key — Read keyboard scancode from PS/2 driver
 * ========================================================================= */

static int64_t sys_get_key(uint64_t *frame) {
    (void)frame;
    return (int64_t)ps2_get_key();
}

/* =========================================================================
 * VGA syscalls — kernel-side VGA operations for userspace shell
 * ========================================================================= */

static int64_t sys_vga_write(uint64_t *frame) {
    const char *buf = (const char *)frame[SC_OFF_RDI / 8];
    uint64_t count  = frame[SC_OFF_RSI / 8];
    if (!validate_user_pointer(buf, count)) return -1;
    for (uint64_t i = 0; i < count; i++) {
        vga_put_str(vga_get_cursor_x(), vga_get_cursor_y(), (char[]){
            buf[i], '\0'}, vga_get_color());
    }
    return (int64_t)count;
}

static int64_t sys_vga_clear(uint64_t *frame) {
    (void)frame;
    vga_clear();
    return 0;
}

static int64_t sys_vga_set_color(uint64_t *frame) {
    uint8_t fg = (uint8_t)frame[SC_OFF_RDI / 8];
    uint8_t bg = (uint8_t)frame[SC_OFF_RSI / 8];
    if (fg > 15 || bg > 15) return -1;
    vga_set_color(fg, bg);
    return 0;
}

static int64_t sys_beep(uint64_t *frame) {
    uint32_t freq = (uint32_t)frame[SC_OFF_RDI / 8];
    uint32_t dur  = (uint32_t)frame[SC_OFF_RSI / 8];
    pcspk_beep(freq, dur);
    return 0;
}

static int64_t sys_reboot(uint64_t *frame) {
    (void)frame;
    /* Flush write-back cache so files survive the reset. */
    bcache_flush_all();
    /* Triple fault reboot via keyboard controller. */
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
    while (1) { asm volatile ("hlt"); }
    return 0;
}

static int64_t sys_vga_cursor_left(uint64_t *frame) {
    (void)frame;
    int x = vga_get_cursor_x();
    int y = vga_get_cursor_y();
    if (x > 0) vga_cursor_set(x - 1, y);
    else if (y > 0) vga_cursor_set(VGA_WIDTH - 1, y - 1);
    return 0;
}

static int64_t sys_vga_cursor_right(uint64_t *frame) {
    (void)frame;
    int x = vga_get_cursor_x();
    int y = vga_get_cursor_y();
    if (x < VGA_WIDTH - 1) vga_cursor_set(x + 1, y);
    else if (y < VGA_HEIGHT - 1) vga_cursor_set(0, y + 1);
    return 0;
}

static int64_t sys_vga_backspace(uint64_t *frame) {
    (void)frame;
    int x = vga_get_cursor_x();
    int y = vga_get_cursor_y();
    if (x == 0 && y == 0) return 0;
    if (x > 0) x--;
    else { x = VGA_WIDTH - 1; y--; }
    vga_put_char(x, y, ' ', vga_get_color());
    vga_cursor_set(x, y);
    vga_flush();
    return 0;
}

static int64_t sys_vga_insert_char(uint64_t *frame) {
    char c = (char)frame[SC_OFF_RDI / 8];
    int x = vga_get_cursor_x();
    int y = vga_get_cursor_y();
    uint8_t color = vga_get_color();
    vga_put_char(x, y, c, color);
    vga_cursor_set(x + 1, y);
    return 0;
}

static int64_t sys_vga_get_cursor(uint64_t *frame) {
    (void)frame;
    return (int64_t)((uint32_t)vga_get_cursor_x() |
                     ((uint32_t)vga_get_cursor_y() << 16));
}

static int64_t sys_vga_set_cursor(uint64_t *frame) {
    int x = (int)frame[SC_OFF_RDI / 8];
    int y = (int)frame[SC_OFF_RSI / 8];
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return -1;
    vga_cursor_set(x, y);
    return 0;
}

static int64_t sys_vga_put_char(uint64_t *frame) {
    int x = (int)frame[SC_OFF_RDI / 8];
    int y = (int)frame[SC_OFF_RSI / 8];
    char c = (char)frame[SC_OFF_RDX / 8];
    uint8_t color = (uint8_t)frame[SC_OFF_R10 / 8];
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return -1;
    vga_put_char(x, y, c, color);
    vga_flush_cell(x, y);
    return 0;
}

static int64_t sys_vga_scroll(uint64_t *frame) {
    int lines = (int)frame[SC_OFF_RDI / 8];
    if (lines <= 0) return -1;
    vga_scroll(lines);
    vga_flush();
    return 0;
}

static int64_t sys_vga_cursor_enable(uint64_t *frame) {
    uint8_t start = (uint8_t)frame[SC_OFF_RDI / 8];
    uint8_t end   = (uint8_t)frame[SC_OFF_RSI / 8];
    vga_cursor_enable(start, end);
    return 0;
}

static int64_t sys_uptime(uint64_t *frame) {
    (void)frame;
    return (int64_t)timer_get_ticks();
}

static int64_t sys_sysinfo(uint64_t *frame) {
    sysinfo_t *user = (sysinfo_t *)frame[SC_OFF_RDI / 8];
    if (!validate_user_pointer(user, sizeof(sysinfo_t))) return -1;

    pmm_stats_t ps;
    pmm_get_stats(&ps);

    sysinfo_t info;
    info.mem_total    = ps.total_frames    * PAGE_SIZE;
    info.mem_free     = ps.free_frames     * PAGE_SIZE;
    info.mem_used     = ps.used_frames     * PAGE_SIZE;
    info.mem_reserved = ps.reserved_frames * PAGE_SIZE;
    info.task_count   = scheduler_get_task_count();
    info.uptime_ticks = timer_get_ticks();

    memcpy(user, &info, sizeof(sysinfo_t));
    return 0;
}

static int64_t sys_shutdown(uint64_t *frame) {
    (void)frame;
    /* Flush write-back cache so pending disk writes survive the reset. */
    bcache_flush_all();
    /* ACPI power button — QEMU i440fx PM1a control port. */
    outw(0x604, 0x2000);
    /* Fallback: q35 PM port, then halt forever. */
    outw(0xB004, 0x2000);
    while (1) { asm volatile ("hlt"); }
    return -1;
}

/* =========================================================================
 * Dispatch table
 * ========================================================================= */

typedef int64_t (*syscall_fn_t)(uint64_t *frame);

static syscall_fn_t syscall_table[] = {
    [SYS_WRITE]   = sys_write,
    [SYS_READ]    = sys_read,
    [SYS_EXIT]    = sys_exit,
    [SYS_FORK]    = sys_fork,
    [SYS_GETPID]  = sys_getpid,
    [SYS_WAIT]    = sys_wait,
    [SYS_GETPPID] = sys_getppid,
    [SYS_BRK]     = sys_brk,
    [SYS_MMAP]    = sys_mmap,
    [SYS_MUNMAP]  = sys_munmap,
    [SYS_EXECVE]  = sys_execve,
    [SYS_OPEN]    = sys_open,
    [SYS_CLOSE]   = sys_close,
    [SYS_FSTAT]   = sys_fstat,
    [SYS_LSEEK]   = sys_lseek,
    [SYS_UNLINK]  = sys_unlink,
    [SYS_GETDENTS]= sys_getdents,
    [SYS_RENAME]  = sys_rename,
    [SYS_USLEEP]  = sys_usleep,
    [SYS_GETTIMEOFDAY] = sys_gettimeofday,
    [SYS_MKDIR]        = sys_mkdir,
    [SYS_RMDIR]        = sys_rmdir,
    [SYS_GET_KEY]      = sys_get_key,
    [SYS_VGA_WRITE]    = sys_vga_write,
    [SYS_VGA_CLEAR]    = sys_vga_clear,
    [SYS_VGA_SET_COLOR]= sys_vga_set_color,
    [SYS_BEEP]         = sys_beep,
    [SYS_REBOOT]       = sys_reboot,
    [SYS_VGA_CURSOR_LEFT]  = sys_vga_cursor_left,
    [SYS_VGA_CURSOR_RIGHT] = sys_vga_cursor_right,
    [SYS_VGA_BACKSPACE]    = sys_vga_backspace,
    [SYS_VGA_INSERT_CHAR]  = sys_vga_insert_char,
    [SYS_VGA_GET_CURSOR]   = sys_vga_get_cursor,
    [SYS_VGA_SET_CURSOR]   = sys_vga_set_cursor,
    [SYS_VGA_PUT_CHAR]     = sys_vga_put_char,
    [SYS_VGA_SCROLL]       = sys_vga_scroll,
    [SYS_VGA_CURSOR_ENABLE]= sys_vga_cursor_enable,
    [SYS_UPTIME]           = sys_uptime,
    [SYS_SYSINFO]          = sys_sysinfo,
    [SYS_SHUTDOWN]         = sys_shutdown,
};

/* =========================================================================
 * C-level syscall dispatcher
 * ========================================================================= */

void syscall_handler(void *frame_ptr) {
    uint64_t *frame = (uint64_t *)frame_ptr;
    int64_t num = (int64_t)frame[SC_OFF_RAX / 8];

    if (num < 0 || num >= SYS_COUNT || !syscall_table[num]) {
        frame[SC_OFF_RAX / 8] = (uint64_t)(-1);
        return;
    }

    int64_t ret = syscall_table[num](frame);
    frame[SC_OFF_RAX / 8] = (uint64_t)ret;
}
