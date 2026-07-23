/**
 * @file task.c
 * @brief Task management implementation.
 *
 * Handles allocation, initialization, and destruction of task control blocks.
 * The initial context is set up so that context_switch() can "return" into
 * the task's entry function.
 */

#include "task.h"
#include "slab.h"
#include "kheap.h"
#include "vmm.h"
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

/** Serial device for logging. */
static serial_dev_t *g_task_serial = NULL;

/** Init task (PID 1) — used for reparenting orphaned children. */
static task_t *g_init_task = NULL;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void task_log(const char *msg) {
    if (g_task_serial) {
        serial_write_string(g_task_serial, msg);
    }
}

/**
 * @brief Set up the initial kernel stack for a new task.
 *
 * Creates a stack frame that context_switch() expects when switching
 * to a task for the first time. The stack is laid out so that the
 * six pops in context_switch load dummy values, and the ret jumps
 * to the entry function.
 *
 * Stack layout (low address → high address):
 *
 *   [+0]  dummy rbx = 0   ← kernel_rsp points here
 *   [+8]  dummy rbp = 0
 *   [+16] dummy r12 = 0
 *   [+24] dummy r13 = 0
 *   [+32] dummy r14 = 0
 *   [+40] dummy r15 = 0
 *   [+48] entry address   ← ret pops this into RIP after 6 pops
 */
static void setup_initial_stack(task_t *task, void (*entry)(void)) {
    /* Start from the top of the kernel stack, aligned to 16 bytes. */
    uint64_t stack_top = task->kernel_stack_base + task->kernel_stack_size;
    stack_top &= ~0xFULL;

    /* Build the initial frame.  context_switch pops 6 registers (48 bytes),
     * then does ret which reads the return address at [+48].  We build
     * from top down so kernel_rsp lands on the rbx slot. */
    uint64_t *sp = (uint64_t *)stack_top;

    sp--; *sp = (uint64_t)entry;  /* return address at [+48] from kernel_rsp */
    sp--; *sp = 0;                /* r15 at [+40] */
    sp--; *sp = 0;                /* r14 at [+32] */
    sp--; *sp = 0;                /* r13 at [+24] */
    sp--; *sp = 0;                /* r12 at [+16] */
    sp--; *sp = 0;                /* rbp at [+8]  */
    sp--; *sp = 0;                /* rbx at [+0]  ← kernel_rsp points here */

    task->kernel_rsp = (uint64_t)sp;
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

    /* Allocate FPU save area (512 bytes, page-aligned from kheap). */
    task->fpu_state = (uint8_t *)kmalloc(512);
    if (!task->fpu_state) {
        task_log("[ERR] task_create: OOM — cannot allocate FPU state\r\n");
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
            slab_free(task);
            return NULL;
        }
        vmm_status_t st = vmm_create_address_space(task->address_space);
        if (st != VMM_OK) {
            task_log("[ERR] task_create: cannot create user address space\r\n");
            slab_free(task->address_space);
            slab_free((void *)task->kernel_stack_base);
            slab_free(task);
            return NULL;
        }

        /* Map a user stack in the new address space. */
        task->user_stack_base = TASK_USER_STACK_BASE;
        task->user_stack_size = TASK_USER_STACK_SIZE;
        /* The user stack pages will be demand-paged. */
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

    /* No zombie child found — return -1 (no wait implementation for blocking yet). */
    return -1;
}
