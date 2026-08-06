/**
 * @file task.c
 * @brief Task management implementation.
 *
 * Handles allocation, initialization, and destruction of task control blocks.
 * The initial context is set up so that context_switch() can "return" into
 * the task's entry function.
 */

#include "task.h"
#include "gdt.h"
#include "slab.h"
#include "kheap.h"
#include "vmm.h"
#include "elf.h"
#include "scheduler.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Static state
 * ========================================================================= */

/** Currently executing task (set by scheduler on context switch). */
static task_t *g_current_task = NULL;

/** Next PID to assign (monotonically increasing). */
static uint64_t g_next_pid = 1;

/* Forward declarations of static helpers (defined below). */
static void task_list_add(task_t *task);
static void task_list_remove(task_t *task);
static void task_close_all_fds(task_t *task);

/** Serial device for logging. */
static serial_dev_t *g_task_serial = NULL;

/** Init task (PID 1) — used for reparenting orphaned children. */
static task_t *g_init_task = NULL;

/** Global linked list of all tasks (for PID lookup). */
static task_t *g_task_list = NULL;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void task_log(const char *msg) {
    if (g_task_serial) {
        serial_write_string(g_task_serial, msg);
    }
}

/**
 * @brief Kernel-mode trampoline that performs IRETQ to ring 3.
 *
 * Declared here so setup_initial_stack can reference it for user tasks.
 * Defined in user_main.c.
 */
extern void user_task_iretq_trampoline(void);

/**
 * @brief Set up the initial kernel stack for a new task.
 *
 * For kernel tasks:
 *   Creates a stack frame that context_switch() expects when switching
 *   to a task for the first time.  context_switch pops 6 registers
 *   then does ret which jumps to the entry function.
 *
 *   Stack layout (low address -> high address):
 *     [+0]  dummy rbx = 0   <- kernel_rsp points here
 *     [+8]  dummy rbp = 0
 *     [+16] dummy r12 = 0
 *     [+24] dummy r13 = 0
 *     [+32] dummy r14 = 0
 *     [+40] dummy r15 = 0
 *     [+48] entry address   <- ret pops this into RIP after 6 pops
 *
 * For user tasks:
 *   Creates an IRETQ frame on the kernel stack, then a context_switch
 *   frame below it.  context_switch pops 6 registers and returns to
 *   user_task_iretq_trampoline, which executes IRETQ to ring 3.  When
 *   the trampoline runs, RSP points at the IRETQ frame bottom (RIP).
 *
 *   Kernel stack layout (kernel_rsp -> high address):
 *     [+0]  rbx = 0                 <- kernel_rsp
 *     [+8]  rbp = 0
 *     [+16] r12 = 0
 *     [+24] r13 = 0
 *     [+32] r14 = 0
 *     [+40] r15 = 0
 *     [+48] ret addr = user_task_iretq_trampoline
 *     [+56] RIP  = entry (0x400000)
 *     [+64] CS   = GDT_USER_CS_SEL (0x23)
 *     [+72] RFLAGS = 0x202 (IF enabled)
 *     [+80] RSP  = user stack top
 *     [+88] SS   = GDT_USER_DS_SEL (0x1B)
 *
 *   Because the stack grows down, the IRETQ qwords are pushed in
 *   reverse order (SS first, then RSP, RFLAGS, CS, RIP) so that RIP
 *   ends up at the lowest address of the frame.
 */
static void setup_initial_stack(task_t *task, void (*entry)(void)) {
    /* Start from the top of the kernel stack, aligned to 16 bytes. */
    uint64_t stack_top = task->kernel_stack_base + task->kernel_stack_size;
    stack_top &= ~0xFULL;

    if (task->is_kernel) {
        /* Kernel task: simple context_switch frame that returns to entry. */
        uint64_t *sp = (uint64_t *)stack_top;

        sp--; *sp = (uint64_t)entry;  /* return address at [+48] from kernel_rsp */
        sp--; *sp = 0;                /* r15 at [+40] */
        sp--; *sp = 0;                /* r14 at [+32] */
        sp--; *sp = 0;                /* r13 at [+24] */
        sp--; *sp = 0;                /* r12 at [+16] */
        sp--; *sp = 0;                /* rbp at [+8]  */
        sp--; *sp = 0;                /* rbx at [+0]  <- kernel_rsp points here */

        task->kernel_rsp = (uint64_t)sp;
    } else {
        /* User task: IRETQ frame on top, context_switch frame below. */
        uint64_t user_stack_top = task->user_stack_base + task->user_stack_size;

        /* --- IRETQ frame (5 qwords; pushed so RIP is at the lowest addr) --- */
        uint64_t *sp = (uint64_t *)stack_top;
        sp--; *sp = GDT_USER_DS_SEL;          /* SS  = 0x1B */
        sp--; *sp = user_stack_top;            /* RSP = user stack top */
        sp--; *sp = 0x202;                     /* RFLAGS (IF=1) */
        sp--; *sp = GDT_USER_CS_SEL;          /* CS  = 0x23 */
        sp--; *sp = (uint64_t)entry;          /* RIP = user entry */

        /* --- Context_switch frame (below IRETQ, returns to trampoline) --- */
        sp--; *sp = (uint64_t)&user_task_iretq_trampoline;  /* ret target */
        sp--; *sp = 0;                /* r15 */
        sp--; *sp = 0;                /* r14 */
        sp--; *sp = 0;                /* r13 */
        sp--; *sp = 0;                /* r12 */
        sp--; *sp = 0;                /* rbp */
        sp--; *sp = 0;                /* rbx <- kernel_rsp points here */

        task->kernel_rsp = (uint64_t)sp;
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void task_init(void *serial_dev) {
    g_task_serial = (serial_dev_t *)serial_dev;
    g_next_pid = 1;
    g_current_task = NULL;

    task_log("[INFO] Task subsystem initialized.\r\n");
}

task_t *task_create(const char *name, void (*entry)(void), bool is_kernel) {
    /* Allocate the PCB from the slab allocator. */
    task_t *task = (task_t *)slab_alloc(sizeof(task_t));
    if (!task) {
        task_log("[ERR] task_create: OOM — cannot allocate PCB\r\n");
        return NULL;
    }

    /* Zero the PCB. */
    memset(task, 0, sizeof(task_t));

    /* Assign PID. */
    task->pid = g_next_pid++;
    task->name = name;
    task->is_kernel = is_kernel;
    task->state = TASK_STATE_CREATED;

    /* Track init task (PID 1) for reparenting. */
    if (task->pid == 1) {
        g_init_task = task;
    }

    /* Scheduling defaults. */
    task->priority = 0;
    task->time_slice = 5;           /* 5 ticks = 50 ms at 100 Hz */
    task->remaining_ticks = task->time_slice;

    /* Lifecycle defaults. */
    task->exit_code = 0;
    task->parent = NULL;
    task->children = NULL;
    task->sibling_next = NULL;

    /* Blocking defaults. */
    task->is_waiting_for_children = false;

    /* Initialize file descriptor table. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        task->fd_table[i].ops = NULL;
        task->fd_table[i].data = NULL;
        task->fd_table[i].refcount = 0;
        task->fd_table[i].flags = 0;
    }

    /* Initialize mmap tracking. */
    task->mmap_count = 0;

    /* Add to global task list. */
    task_list_add(task);

    /* Allocate FPU save area (512 bytes, page-aligned from kheap). */
    task->fpu_state = (uint8_t *)kmalloc(512);
    if (!task->fpu_state) {
        task_log("[ERR] task_create: OOM — cannot allocate FPU state\r\n");
        task_list_remove(task);
        slab_free(task);
        return NULL;
    }
    /* Initialize FPU and save default state. */
    __asm__ volatile ("fninit");
    __asm__ volatile ("fxsave (%0)" : : "r" (task->fpu_state) : "memory");

    /* Allocate kernel stack. */
    task->kernel_stack_size = TASK_KERNEL_STACK_SIZE;
    task->kernel_stack_base = (uint64_t)slab_alloc(TASK_KERNEL_STACK_SIZE);
    if (!task->kernel_stack_base) {
        task_log("[ERR] task_create: OOM — cannot allocate kernel stack\r\n");
        kfree(task->fpu_state);
        task_list_remove(task);
        slab_free(task);
        return NULL;
    }
    memset((void *)task->kernel_stack_base, 0, TASK_KERNEL_STACK_SIZE);

    /* Set up the address space.
     * Kernel tasks share the kernel address space.
     * User tasks get their own address space (copied from kernel). */
    if (is_kernel) {
        task->address_space = vmm_get_kernel_address_space();
    } else {
        /* Allocate and initialize a user address space. */
        task->address_space = (address_space_t *)slab_alloc(sizeof(address_space_t));
        if (!task->address_space) {
            task_log("[ERR] task_create: OOM — cannot allocate address space struct\r\n");
            slab_free((void *)task->kernel_stack_base);
            kfree(task->fpu_state);
            task_list_remove(task);
            slab_free(task);
            return NULL;
        }
        vmm_status_t st = vmm_create_address_space(task->address_space);
        if (st != VMM_OK) {
            task_log("[ERR] task_create: cannot create user address space\r\n");
            slab_free(task->address_space);
            slab_free((void *)task->kernel_stack_base);
            kfree(task->fpu_state);
            task_list_remove(task);
            slab_free(task);
            return NULL;
        }

        /* Map a user stack in the new address space using DEMAND PTEs.
         * Pages are lazily allocated on first access (page fault). */
        task->user_stack_base = TASK_USER_STACK_BASE;
        task->user_stack_size = TASK_USER_STACK_SIZE;
        uint64_t stack_flags = VMM_FLAG_WRITE | VMM_FLAG_USER;
        for (uint64_t va = task->user_stack_base;
             va < task->user_stack_base + task->user_stack_size;
             va += PAGE_SIZE) {
            vmm_map_demand_page(task->address_space, va, stack_flags);
        }

        /* Initialize heap — will be set properly by execve/task_create_elf. */
        task->user_heap_start = 0;
        task->user_heap_brk = 0;
    }

    /* Set up the initial kernel stack for context_switch. */
    setup_initial_stack(task, entry);

    task_log("[INFO] Task created: PID=");
    {
        char buf[21];
        int i = 0;
        uint64_t pid = task->pid;
        if (pid == 0) { buf[i++] = '0'; }
        else {
            char tmp[21];
            int j = 0;
            while (pid > 0) { tmp[j++] = '0' + (pid % 10); pid /= 10; }
            while (j > 0) { buf[i++] = tmp[--j]; }
        }
        buf[i] = '\0';
        task_log(buf);
    }
    task_log(" name=");
    task_log(name);
    task_log("\r\n");

    return task;
}

task_t *task_create_idle(void) {
    /* The idle task runs a hlt loop to save power when no other tasks
     * are ready. It uses a minimal 4 KiB kernel stack. */

    task_t *idle = (task_t *)slab_alloc(sizeof(task_t));
    if (!idle) return NULL;
    memset(idle, 0, sizeof(task_t));

    idle->pid = 0;  /* PID 0 is always the idle task */
    idle->name = "idle";
    idle->is_kernel = true;
    idle->state = TASK_STATE_READY;
    idle->priority = 0;
    idle->time_slice = 1;
    idle->remaining_ticks = idle->time_slice;
    idle->is_waiting_for_children = false;

    /* Minimal kernel stack (4 KiB is enough for a hlt loop). */
    idle->kernel_stack_size = 4096;
    idle->kernel_stack_base = (uint64_t)slab_alloc(4096);
    if (!idle->kernel_stack_base) {
        slab_free(idle);
        return NULL;
    }
    memset((void *)idle->kernel_stack_base, 0, 4096);

    idle->address_space = vmm_get_kernel_address_space();

    /* Allocate FPU save area for idle task. */
    idle->fpu_state = (uint8_t *)kmalloc(512);
    if (!idle->fpu_state) {
        slab_free((void *)idle->kernel_stack_base);
        slab_free(idle);
        return NULL;
    }
    __asm__ volatile ("fninit");
    __asm__ volatile ("fxsave (%0)" : : "r" (idle->fpu_state) : "memory");

    /* Set up initial stack pointing to the idle entry. */
    extern void idle_task_entry(void);
    setup_initial_stack(idle, idle_task_entry);

    task_log("[INFO] Idle task created (PID=0).\r\n");
    return idle;
}

void task_destroy(task_t *task) {
    if (!task) return;

    /* Close all open file descriptors. */
    task_close_all_fds(task);

    /* Remove from global task list. */
    task_list_remove(task);

    task_log("[INFO] Destroying task PID=");
    {
        char buf[21];
        int i = 0;
        uint64_t pid = task->pid;
        if (pid == 0) { buf[i++] = '0'; }
        else {
            char tmp[21];
            int j = 0;
            while (pid > 0) { tmp[j++] = '0' + (pid % 10); pid /= 10; }
            while (j > 0) { buf[i++] = tmp[--j]; }
        }
        buf[i] = '\0';
        task_log(buf);
    }
    task_log("\r\n");

    /* Free user address space if not kernel task. */
    if (!task->is_kernel && task->address_space) {
        vmm_destroy_address_space(task->address_space);
        slab_free(task->address_space);
    }

    /* Free FPU state buffer. */
    if (task->fpu_state) {
        kfree(task->fpu_state);
    }

    /* Free kernel stack. */
    if (task->kernel_stack_base) {
        slab_free((void *)task->kernel_stack_base);
    }

    /* Free the PCB. */
    task->state = TASK_STATE_DEAD;
    slab_free(task);
}

task_t *task_get_current(void) {
    return g_current_task;
}

void task_set_current(task_t *task) {
    g_current_task = task;
}

uint64_t task_get_next_pid(void) {
    return g_next_pid;
}

/* =========================================================================
 * Global Task List
 * ========================================================================= */

static void task_list_add(task_t *task) {
    task->g_next = g_task_list;
    g_task_list = task;
}

static void task_list_remove(task_t *task) {
    task_t **pp = &g_task_list;
    while (*pp) {
        if (*pp == task) {
            *pp = task->g_next;
            task->g_next = NULL;
            return;
        }
        pp = &(*pp)->g_next;
    }
}

task_t *task_find_by_pid(uint64_t pid) {
    task_t *t = g_task_list;
    while (t) {
        if (t->pid == pid) return t;
        t = t->g_next;
    }
    return NULL;
}

task_t *task_get_list_head(void) {
    return g_task_list;
}


/* =========================================================================
 * Sleep / Wake
 * ========================================================================= */

void task_wake(task_t *task) {
    if (!task) return;
    if (task->state != TASK_STATE_BLOCKED) return;

    task->is_waiting_for_children = false;
    scheduler_wake_task(task);
}

/* =========================================================================
 * File Descriptor Management
 * ========================================================================= */

static void task_close_all_fds(task_t *task) {
    if (!task) return;
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        file_t *f = &task->fd_table[i];
        if (f->ops && f->ops->close) {
            f->ops->close(i, f->data);
        }
        if (f->data) {
            /* Decrement refcount; if zero, caller should free.
             * For now, we don't have a central file table — just clear. */
            f->data = NULL;
        }
        f->ops = NULL;
        f->refcount = 0;
        f->flags = 0;
    }
}

char task_state_char(task_state_t state) {
    switch (state) {
        case TASK_STATE_CREATED:  return 'C';
        case TASK_STATE_READY:    return 'R';
        case TASK_STATE_RUNNING:  return 'X';
        case TASK_STATE_BLOCKED:  return 'B';
        case TASK_STATE_SLEEPING: return 'S';
        case TASK_STATE_ZOMBIE:   return 'Z';
        case TASK_STATE_DEAD:     return 'D';
        default:                  return '?';
    }
}

/* =========================================================================
 * Process lifecycle
 * ========================================================================= */

/**
 * @brief Get the init task (PID 1) for reparenting.
 *
 * Walks the task list... actually, we don't have a global task list.
 * For now, we store a pointer to init during task_exit if it's the
 * first call.  A better approach: store init_task globally.
 *
 * NOTE: This is a simplification.  In production, init is registered
 * once and stored in a global.  For now, we use a static pointer.
 */

void task_reparent_children(task_t *task) {
    if (!task || !task->children) return;

    /* Find init task (PID 1) to reparent to. */
    task_t *init = g_init_task;
    if (!init) {
        /* Fallback: just orphan the children (they'll be reaped by scheduler). */
        task_t *child = task->children;
        while (child) {
            task_t *next = child->sibling_next;
            child->parent = NULL;
            child->sibling_next = NULL;
            child = next;
        }
        task->children = NULL;
        return;
    }

    /* Move all children to init's children list. */
    task_t *child = task->children;
    while (child) {
        task_t *next = child->sibling_next;
        child->parent = init;
        child->sibling_next = init->children;
        init->children = child;
        child = next;
    }
    task->children = NULL;
}

void task_exit(int code) {
    task_t *cur = g_current_task;
    if (!cur) return;

    task_log("[TASK] exit PID=");
    {
        char buf[21];
        int i = 0;
        uint64_t pid = cur->pid;
        if (pid == 0) { buf[i++] = '0'; }
        else {
            char tmp[21];
            int j = 0;
            while (pid > 0) { tmp[j++] = '0' + (pid % 10); pid /= 10; }
            while (j > 0) { buf[i++] = tmp[--j]; }
        }
        buf[i] = '\0';
        task_log(buf);
    }
    task_log(" code=");
    {
        char buf[12];
        int i = 0;
        int val = code;
        if (val < 0) { buf[i++] = '-'; val = -val; }
        if (val == 0) { buf[i++] = '0'; }
        else {
            char tmp[12];
            int j = 0;
            while (val > 0) { tmp[j++] = '0' + (val % 10); val /= 10; }
            while (j > 0) { buf[i++] = tmp[--j]; }
        }
        buf[i] = '\0';
        task_log(buf);
    }
    task_log("\r\n");

    /* Record exit code. */
    cur->exit_code = code;

    /* Reparent children to init. */
    task_reparent_children(cur);

    /* Mark as zombie — scheduler will remove from MLFQ,
     * but task_destroy is deferred until parent calls task_wait. */
    cur->state = TASK_STATE_ZOMBIE;

    /* If parent is waiting for children, wake it up. */
    if (cur->parent && cur->parent->is_waiting_for_children) {
        task_wake(cur->parent);
    }

    /* If this is the init task exiting, we have a problem.
     * In a real OS this is a panic.  For now, just halt. */
    if (cur->pid == 1) {
        task_log("[TASK] Init task exited! Halting.\r\n");
        while (1) { asm volatile ("cli; hlt"); }
    }
}

int task_wait(int *status) {
    task_t *cur = g_current_task;
    if (!cur) return -1;

    /* Search for a zombie child. */
    task_t **pp = &cur->children;
    while (*pp) {
        task_t *child = *pp;
        if (child->state == TASK_STATE_ZOMBIE) {
            /* Found a zombie child — reap it. */
            int code = child->exit_code;
            if (status) *status = code;

            /* Remove from children list. */
            *pp = child->sibling_next;

            task_log("[TASK] wait: reaping PID=");
            {
                char buf[21];
                int i = 0;
                uint64_t pid = child->pid;
                if (pid == 0) { buf[i++] = '0'; }
                else {
                    char tmp[21];
                    int j = 0;
                    while (pid > 0) { tmp[j++] = '0' + (pid % 10); pid /= 10; }
                    while (j > 0) { buf[i++] = tmp[--j]; }
                }
                buf[i] = '\0';
                task_log(buf);
            }
            task_log("\r\n");

            /* Save child PID before destroying (destroy frees the PCB). */
            int child_pid = (int)child->pid;

            /* Free the child's resources. */
            task_destroy(child);

            return child_pid;
        }
        pp = &child->sibling_next;
    }

    /* No zombie child found — block if there are living children. */
    if (cur->children) {
        cur->is_waiting_for_children = true;
        scheduler_unqueue_task(cur);
        cur->state = TASK_STATE_BLOCKED;

        return -1;  /* Caller (syscall handler) will context switch */
    }

    /* No children at all — return error. */
    return -1;
}

/* =========================================================================
 * ELF Task Creation
 * ========================================================================= */

task_t *task_create_elf(const char *name, const void *elf_data, size_t elf_size) {
    if (!elf_data || elf_size < sizeof(elf64_header_t)) {
        task_log("[ERR] task_create_elf: invalid ELF data\r\n");
        return NULL;
    }

    /* Allocate the PCB from the slab allocator. */
    task_t *task = (task_t *)slab_alloc(sizeof(task_t));
    if (!task) {
        task_log("[ERR] task_create_elf: OOM — cannot allocate PCB\r\n");
        return NULL;
    }
    memset(task, 0, sizeof(task_t));

    /* Assign PID. */
    task->pid = g_next_pid++;
    task->name = name;
    task->is_kernel = false;
    task->state = TASK_STATE_CREATED;

    /* Track init task. */
    if (task->pid == 1) {
        g_init_task = task;
    }

    /* Scheduling defaults. */
    task->priority = 0;
    task->time_slice = 5;
    task->remaining_ticks = task->time_slice;

    /* Lifecycle defaults. */
    task->exit_code = 0;
    task->parent = NULL;
    task->children = NULL;
    task->sibling_next = NULL;

    /* Blocking defaults. */
    task->is_waiting_for_children = false;

    /* Initialize file descriptor table. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        task->fd_table[i].ops = NULL;
        task->fd_table[i].data = NULL;
        task->fd_table[i].refcount = 0;
        task->fd_table[i].flags = 0;
    }

    /* Initialize mmap tracking. */
    task->mmap_count = 0;

    /* Add to global task list. */
    task_list_add(task);

    /* Allocate FPU save area. */
    task->fpu_state = (uint8_t *)kmalloc(512);
    if (!task->fpu_state) {
        task_log("[ERR] task_create_elf: OOM — cannot allocate FPU state\r\n");
        task_list_remove(task);
        slab_free(task);
        return NULL;
    }
    __asm__ volatile ("fninit");
    __asm__ volatile ("fxsave (%0)" : : "r" (task->fpu_state) : "memory");

    /* Allocate kernel stack. */
    task->kernel_stack_size = TASK_KERNEL_STACK_SIZE;
    task->kernel_stack_base = (uint64_t)slab_alloc(TASK_KERNEL_STACK_SIZE);
    if (!task->kernel_stack_base) {
        task_log("[ERR] task_create_elf: OOM — cannot allocate kernel stack\r\n");
        kfree(task->fpu_state);
        task_list_remove(task);
        slab_free(task);
        return NULL;
    }
    memset((void *)task->kernel_stack_base, 0, TASK_KERNEL_STACK_SIZE);

    /* Create user address space. */
    task->address_space = (address_space_t *)slab_alloc(sizeof(address_space_t));
    if (!task->address_space) {
        task_log("[ERR] task_create_elf: OOM — cannot allocate address space\r\n");
        slab_free((void *)task->kernel_stack_base);
        kfree(task->fpu_state);
        task_list_remove(task);
        slab_free(task);
        return NULL;
    }
    vmm_status_t st = vmm_create_address_space(task->address_space);
    if (st != VMM_OK) {
        task_log("[ERR] task_create_elf: cannot create user address space\r\n");
        slab_free(task->address_space);
        slab_free((void *)task->kernel_stack_base);
        kfree(task->fpu_state);
        task_list_remove(task);
        slab_free(task);
        return NULL;
    }

    /* Load ELF segments into the address space. */
    elf_load_result_t elf_result;
    elf_status_t elf_st = elf_load(elf_data, elf_size,
                                    task->address_space, &elf_result);
    if (elf_st != ELF_OK) {
        task_log("[ERR] task_create_elf: ELF load failed\r\n");
        vmm_destroy_address_space(task->address_space);
        slab_free(task->address_space);
        slab_free((void *)task->kernel_stack_base);
        kfree(task->fpu_state);
        task_list_remove(task);
        slab_free(task);
        return NULL;
    }

    /* Set up user stack (demand-paged). */
    task->user_stack_base = TASK_USER_STACK_BASE;
    task->user_stack_size = TASK_USER_STACK_SIZE;
    uint64_t stack_flags = VMM_FLAG_WRITE | VMM_FLAG_USER;
    for (uint64_t va = task->user_stack_base;
         va < task->user_stack_base + task->user_stack_size;
         va += PAGE_SIZE) {
        vmm_map_demand_page(task->address_space, va, stack_flags);
    }

    /* Set up user heap — starts after the highest loaded ELF segment,
     * page-aligned.  Heap grows via brk syscall. */
    task->user_heap_start = (elf_result.top + PAGE_SIZE - 1)
                          & ~(PAGE_SIZE - 1);
    task->user_heap_brk = task->user_heap_start;

    /* Set up initial kernel stack for context_switch → iretq to user mode. */
    setup_initial_stack(task, (void (*)(void))elf_result.entry);

    task_log("[INFO] ELF task created: PID=");
    {
        char buf[21];
        int i = 0;
        uint64_t pid = task->pid;
        if (pid == 0) { buf[i++] = '0'; }
        else {
            char tmp[21];
            int j = 0;
            while (pid > 0) { tmp[j++] = '0' + (pid % 10); pid /= 10; }
            while (j > 0) { buf[i++] = tmp[--j]; }
        }
        buf[i] = '\0';
        task_log(buf);
    }
    task_log(" name=");
    task_log(name);
    task_log(" entry=");
    {
        char buf[19];
        static const char hex[] = "0123456789abcdef";
        buf[0] = '0'; buf[1] = 'x';
        for (int k = 15; k >= 0; k--) {
            buf[2 + (15 - k)] = hex[(elf_result.entry >> (k * 4)) & 0xF];
        }
        buf[18] = '\0';
        task_log(buf);
    }
    task_log("\r\n");

    return task;
}
