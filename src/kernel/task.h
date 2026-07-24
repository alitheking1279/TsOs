/**
 * @file task.h
 * @brief Task (process/thread) management — PCB structure and lifecycle.
 *
 * Each task is represented by a task_t (Process Control Block) that stores:
 *   - CPU context (saved RSP, callee-saved registers) for context switching.
 *   - Memory context (address space pointer for CR3 switching).
 *   - Scheduling metadata (state, priority, time quantum).
 *   - A kernel stack for syscalls and interrupts while in this task.
 *
 * Tasks transition through states:
 *   CREATED → READY → RUNNING → (READY | BLOCKED | SLEEPING) → ZOMBIE → DEAD
 *
 * Task creation:
 *   task_create() allocates a PCB from the slab allocator, allocates
 *   a kernel stack, and sets up an initial interrupt frame on that stack
 *   so the context-switch code can "return" into the task's entry point.
 *
 * Context switch contract:
 *   When task_A is switched out, its kernel RSP is saved in task_A->kernel_rsp.
 *   When task_B is switched in, its kernel RSP is loaded from task_B->kernel_rsp.
 *   The context switch code only saves/restores callee-saved registers
 *   (rbx, rbp, r12-r15) and swaps RSP. Caller-saved registers are already
 *   saved in the interrupt frame on the stack.
 */

#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "page_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Default kernel stack size for each task (32 KiB). */
#define TASK_KERNEL_STACK_SIZE   (32 * 1024)

/** Default user stack size (64 KiB). */
#define TASK_USER_STACK_SIZE     (64 * 1024)

/** Virtual address base for user stacks (in lower-half user space). */
#define TASK_USER_STACK_BASE     0x0000004000000000ULL

/** Default user heap base address (set by execve after loading ELF). */
#define TASK_USER_HEAP_BASE      0x0000000080000000ULL

/** Invalid PID (sentinel). */
#define TASK_INVALID_PID         0

/** Maximum number of open file descriptors per task. */
#define TASK_MAX_FDS             64

/** Maximum number of mmap regions per task. */
#define TASK_MAX_MMAPS           16

/* =========================================================================
 * File Operations (minimal VFS interface)
 * ========================================================================= */

/** File operations vtable — one per open file. */
typedef struct file_ops {
    int64_t (*read)(int fd, void *buf, uint64_t count, void *data);
    int64_t (*write)(int fd, const void *buf, uint64_t count, void *data);
    void    (*close)(int fd, void *data);
} file_ops_t;

/** Open file descriptor entry. */
typedef struct file {
    file_ops_t *ops;
    void       *data;
    int         refcount;
    uint32_t    flags;
} file_t;

/* =========================================================================
 * Memory Map Region Tracking
 * ========================================================================= */

/** Describes a mmap'd region. */
typedef struct mmap_region {
    uint64_t base;
    uint64_t size;
    uint64_t prot;
} mmap_region_t;

/* =========================================================================
 * Task States
 * ========================================================================= */

typedef enum {
    TASK_STATE_CREATED  = 0,  /**< Just created, not yet schedulable. */
    TASK_STATE_READY    = 1,  /**< In the ready queue, waiting for CPU. */
    TASK_STATE_RUNNING  = 2,  /**< Currently executing on the CPU. */
    TASK_STATE_BLOCKED  = 3,  /**< Waiting for an event (not schedulable). */
    TASK_STATE_SLEEPING = 4,  /**< Voluntarily sleeping (not schedulable). */
    TASK_STATE_ZOMBIE   = 5,  /**< Exited, resources not yet reclaimed. */
    TASK_STATE_DEAD     = 6   /**< Fully destroyed. */
} task_state_t;

/* =========================================================================
 * Task Control Block (PCB)
 * ========================================================================= */

/**
 * @brief Process Control Block — one per task/thread.
 *
 * Aligned to 16 bytes so that RSP stays 16-byte aligned as required
 * by the System V AMD64 ABI.
 */
typedef struct task {
    /* ---- CPU context (saved/restored by context switch on kernel stack) ---- */
    uint64_t kernel_rsp;        /**< Saved kernel stack pointer. */

    /* ---- Scheduling metadata ---- */
    uint64_t        pid;            /**< Process ID (unique, monotonically increasing). */
    task_state_t    state;          /**< Current state. */
    uint64_t        priority;       /**< Priority (0 = normal, higher = more urgent). */
    uint64_t        time_slice;     /**< Quantum in ticks (reloaded on schedule). */
    uint64_t        remaining_ticks;/**< Ticks remaining in current quantum. */
    int             mlfq_level;     /**< MLFQ priority level (0=highest, 3=lowest). */

    /* ---- Process lifecycle ---- */
    int             exit_code;      /**< Exit code (valid when state == ZOMBIE). */
    struct task     *parent;        /**< Parent process (NULL for init/kernel tasks). */
    struct task     *children;      /**< Head of children linked list (via sibling_next). */
    struct task     *sibling_next;  /**< Next sibling in parent's children list. */

    /* ---- Memory context ---- */
    address_space_t *address_space; /**< Pointer to this task's address space (for CR3). */
    uint64_t        kernel_stack_base;  /**< Base address of kernel stack (for freeing). */
    uint64_t        kernel_stack_size;  /**< Size of kernel stack. */
    uint64_t        user_stack_base;    /**< Base address of user stack. */
    uint64_t        user_stack_size;    /**< Size of user stack. */
    uint64_t        user_heap_start;    /**< Base of user heap (set by execve). */
    uint64_t        user_heap_brk;      /**< Current heap break (grows on brk). */

    /* ---- FPU state (saved/restored by context_switch on kernel stack) ---- */
    uint8_t         *fpu_state;     /**< Aligned 512-byte FXSAVE area, or NULL for kernel tasks. */

    /* ---- Scheduler linkage ---- */
    struct task     *next;          /**< Next task in the ready/sleep queue. */

    /* ---- Global task list ---- */
    struct task     *g_next;        /**< Next task in the global task list. */

    /* ---- Blocking ---- */
    bool            is_waiting_for_children; /**< True if blocked in task_wait. */

    /* ---- File descriptor table ---- */
    file_t          fd_table[TASK_MAX_FDS];

    /* ---- Memory map tracking ---- */
    mmap_region_t   mmap_regions[TASK_MAX_MMAPS];
    int             mmap_count;

    /* ---- Metadata ---- */
    const char      *name;          /**< Human-readable task name (for debugging). */
    bool            is_kernel;      /**< true = kernel-mode task, false = user-mode. */
} __attribute__((aligned(16))) task_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the task management subsystem.
 *
 * Resets the PID counter and initializes internal structures.
 * Must be called before task_create().
 *
 * @param serial_dev  Serial device for logging. May be NULL.
 */
void task_init(void *serial_dev);

/**
 * @brief Create a new kernel-mode task.
 *
 * Allocates a PCB, kernel stack, and sets up an initial context
 * so the task will start executing at `entry` when first scheduled.
 *
 * @param name       Human-readable name (stored by pointer, not copied).
 * @param entry      Entry point function (called with no arguments).
 * @param is_kernel  true for kernel-mode task.
 * @return Pointer to the new task, or NULL on allocation failure.
 */
task_t *task_create(const char *name, void (*entry)(void), bool is_kernel);

/**
 * @brief Create a user-mode task from an ELF64 executable.
 *
 * Allocates a PCB, address space, kernel stack, and loads the ELF
 * segments into the address space.  The user stack and heap are
 * demand-paged.
 *
 * @param name      Human-readable name.
 * @param elf_data  Pointer to the ELF file data (must remain valid).
 * @param elf_size  Size of the ELF data in bytes.
 * @return Pointer to the new task, or NULL on failure.
 */
task_t *task_create_elf(const char *name, const void *elf_data, size_t elf_size);

/**
 * @brief Create the idle task (called by scheduler_init).
 *
 * The idle task runs when no other tasks are ready. It executes
 * a `hlt` loop to save power.
 *
 * @return Pointer to the idle task.
 */
task_t *task_create_idle(void);

/**
 * @brief Destroy a task and free all its resources.
 *
 * Frees the kernel stack, address space (if user task), and the PCB.
 * The task must not be the currently running task.
 *
 * @param task  Task to destroy.
 */
void task_destroy(task_t *task);

/**
 * @brief Get the currently running task.
 *
 * @return Pointer to the current task_t, or NULL if no task is running.
 */
task_t *task_get_current(void);

/**
 * @brief Set the currently running task.
 *
 * @param task  Task to mark as current.
 */
void task_set_current(task_t *task);

/**
 * @brief Get the next PID to be assigned.
 *
 * Useful for testing — verifies PID uniqueness.
 *
 * @return Next PID that task_create() will assign.
 */
uint64_t task_get_next_pid(void);

/**
 * @brief Convert a task_state_t to a printable character.
 *
 * @param state  Task state.
 * @return Single character: 'C', 'R', 'X', 'B', 'S', 'Z', 'D'.
 */
char task_state_char(task_state_t state);

/**
 * @brief Exit the current task (sys_exit implementation).
 *
 * Sets the task to ZOMBIE state, records exit_code, reparents
 * children to the init task, and notifies the parent if waiting.
 * Does NOT free the task — that is done by task_wait/task_reap.
 *
 * @param code  Exit code (stored in task->exit_code).
 */
void task_exit(int code);

/**
 * @brief Wait for a child task to exit (sys_waitpid implementation).
 *
 * Blocks the current task until a child exits.  When a child
 * exits (becomes ZOMBIE), its exit_code is stored in *status
 * (if non-NULL), the child is reaped (freed), and this function
 * returns the child's PID.  Returns -1 if no children exist.
 *
 * @param status  Pointer to store child's exit code (may be NULL).
 * @return Child's PID, or -1 if no children.
 */
int task_wait(int *status);

/**
 * @brief Reparent all children of a task to the init task (PID 1).
 *
 * Called during task_exit to ensure no task becomes an orphan
 * without a parent to reap it.
 *
 * @param task  Task whose children to reparent.
 */
void task_reparent_children(task_t *task);

/**
 * @brief Wake a blocked task — make it ready and re-enqueue in the scheduler.
 *
 * Called by task_exit when a child exits and the parent is waiting.
 *
 * @param task  Task to wake (must be in TASK_STATE_BLOCKED).
 */
void task_wake(task_t *task);

/**
 * @brief Find a task by PID using the global task list.
 *
 * @param pid  Process ID to find.
 * @return Pointer to the task, or NULL if not found.
 */
task_t *task_find_by_pid(uint64_t pid);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_TASK_H */
