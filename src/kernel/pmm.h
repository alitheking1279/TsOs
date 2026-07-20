/**
 * @file pmm.h
 * @brief Physical Memory Manager (PMM) — bitmap-based 4 KiB page allocator.
 *
 * Design overview
 * ===============
 * The PMM tracks physical memory at 4 KiB (PAGE_SIZE) granularity using a
 * statically allocated bitmap:
 *
 *   bit = 0  →  frame is FREE
 *   bit = 1  →  frame is USED / reserved
 *
 * All frames start USED.  pmm_init() then walks the Multiboot2 memory map
 * and marks each usable RAM region as FREE, then re-marks:
 *   • physical frame 0          (protect the real-mode IVT / BIOS data)
 *   • the kernel image itself   [_kernel_start … _kernel_end)
 *   • the PMM bitmap itself     (the bitmap lives in .bss)
 *
 * Allocation policy
 * -----------------
 * pmm_alloc_frame()  — linear first-fit scan; O(N/64) with 64-bit words.
 * pmm_free_frame()   — O(1) single-bit clear.
 * pmm_alloc_frames() — contiguous N-frame allocation; O(N²/64) worst case
 *                      but fine for early-boot use.
 *
 * Limits
 * ------
 * PMM_MAX_FRAMES covers 4 GiB physical memory (1 M frames × 4 KiB).
 * The bitmap itself costs 128 KiB of BSS — acceptable for a kernel.
 *
 * Multiboot2 memory map
 * ---------------------
 * The PMM parses the Multiboot2 info structure passed in EBX at boot,
 * forwarded via kernel_main's info_ptr parameter.  Only entries of type
 * MULTIBOOT_MEMORY_AVAILABLE (1) are made free.
 *
 * References
 * ----------
 *   Multiboot2 specification §3.6.8 — memory map tag
 *   Intel SDM Vol.3A §5.3 — physical memory organization
 */

#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Physical page size — 4 KiB, the smallest addressable physical frame. */
#define PAGE_SIZE           4096U

/** Log₂(PAGE_SIZE): used for fast shift arithmetic. */
#define PAGE_SHIFT          12U

/** Maximum number of physical frames tracked by the PMM.
 *  1 M frames × 4 KiB = 4 GiB physical address space. */
#define PMM_MAX_FRAMES      (1U << 20)   /* 1,048,576 frames */

/** Bits per bitmap word (uint64_t). */
#define PMM_BITS_PER_WORD   64U

/** Number of 64-bit words in the bitmap. */
#define PMM_BITMAP_WORDS    (PMM_MAX_FRAMES / PMM_BITS_PER_WORD)

/* =========================================================================
 * PMM status codes
 * ========================================================================= */

typedef enum {
    PMM_OK              =  0,   /**< Success. */
    PMM_ERR_OOM         = -1,   /**< Out of memory: no free frames. */
    PMM_ERR_INVALID     = -2,   /**< Invalid argument (e.g. NULL, bad addr). */
    PMM_ERR_ALREADY_FREE= -3,   /**< Double-free detected. */
    PMM_ERR_NOT_INIT    = -4,   /**< pmm_init() was not called. */
} pmm_status_t;

/* =========================================================================
 * PMM statistics (populated by pmm_init, updated on alloc/free)
 * ========================================================================= */

typedef struct {
    uint64_t total_frames;      /**< Total tracked frames (phys mem / 4KiB). */
    uint64_t free_frames;       /**< Number of currently free frames. */
    uint64_t used_frames;       /**< Number of currently used/reserved frames. */
    uint64_t reserved_frames;   /**< Frames reserved at init (kernel + bitmap). */
} pmm_stats_t;

/* =========================================================================
 * Multiboot2 structures (minimal subset needed by the PMM)
 *
 * The full Multiboot2 spec is at https://www.gnu.org/software/grub/manual/multiboot2/
 * We only parse the tags we need — anything else is skipped.
 * ========================================================================= */

/** Multiboot2 info structure fixed header (8 bytes). */
typedef struct {
    uint32_t total_size;    /**< Total size of the info structure in bytes. */
    uint32_t reserved;
} __attribute__((packed)) mb2_header_t;

/** Multiboot2 tag header (present at the start of every tag). */
typedef struct {
    uint32_t type;          /**< Tag type identifier. */
    uint32_t size;          /**< Total size of this tag in bytes. */
} __attribute__((packed)) mb2_tag_t;

/** Multiboot2 memory-map entry (variable stride — use entry_size). */
typedef struct {
    uint64_t base_addr;     /**< Physical start of this memory region. */
    uint64_t length;        /**< Length of this memory region in bytes. */
    uint32_t type;          /**< Region type (1 = available RAM). */
    uint32_t reserved;
} __attribute__((packed)) mb2_mmap_entry_t;

/** Multiboot2 memory-map tag (type 6). */
typedef struct {
    uint32_t type;          /**< = 6 */
    uint32_t size;
    uint32_t entry_size;    /**< Size of each mb2_mmap_entry_t (may be > 24). */
    uint32_t entry_version; /**< = 0 */
    /* Entries follow immediately after, each `entry_size` bytes wide. */
} __attribute__((packed)) mb2_mmap_tag_t;

/** Tag type value for the memory map tag. */
#define MB2_TAG_TYPE_MMAP       6U

/** Multiboot2 memory region type: usable RAM. */
#define MB2_MMAP_AVAILABLE      1U

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the PMM from the Multiboot2 memory map.
 *
 * Steps:
 *   1. Start with ALL frames marked USED.
 *   2. Parse the Multiboot2 info structure at `mb2_info_phys` for the
 *      memory-map tag (type 6).
 *   3. For each AVAILABLE entry, mark frames as FREE.
 *   4. Re-mark frame 0 as USED (protects BIOS/IVT).
 *   5. Re-mark the kernel image [_kernel_start … _kernel_end) as USED.
 *   6. Re-mark the PMM bitmap itself as USED.
 *   7. Emit a detailed serial log of the memory map and final counts.
 *
 * @param mb2_info_phys  32-bit physical address of the Multiboot2 info struct.
 * @param serial         Initialized serial device for logging. May be NULL
 *                       (logging is suppressed).
 */
void pmm_init(uint32_t mb2_info_phys, void *serial);

/**
 * @brief Allocate one physical 4 KiB frame (first-fit).
 *
 * @return Physical address of the allocated frame (4 KiB aligned),
 *         or 0 on failure (out of memory or not initialized).
 *         Address 0 is never returned for a valid allocation because
 *         frame 0 is always reserved.
 */
uint64_t pmm_alloc_frame(void);

/**
 * @brief Allocate `count` contiguous physical frames.
 *
 * All `count` frames are guaranteed to be physically contiguous.
 * Returns the base address of the first frame, or 0 on failure.
 *
 * @param count  Number of contiguous 4 KiB pages to allocate (>= 1).
 */
uint64_t pmm_alloc_frames(uint64_t count);

/**
 * @brief Free a previously allocated physical frame.
 *
 * @param addr  Physical address of the frame to free.  Must be 4 KiB aligned
 *              and must have been returned by pmm_alloc_frame() or
 *              pmm_alloc_frames().
 * @return PMM_OK on success.
 *         PMM_ERR_INVALID if addr is not page-aligned or out of range.
 *         PMM_ERR_ALREADY_FREE if the frame was already free (double-free).
 */
pmm_status_t pmm_free_frame(uint64_t addr);

/**
 * @brief Mark a physical frame as USED without going through the allocator.
 *
 * Used internally by pmm_init() and available to other kernel subsystems
 * that need to reserve specific physical frames (e.g. MMIO regions).
 *
 * @param addr  Physical address of the frame (4 KiB aligned).
 */
void pmm_mark_used(uint64_t addr);

/**
 * @brief Mark a physical frame as FREE without going through the allocator.
 *
 * Use with care — marking an already-in-use frame as free causes corruption.
 *
 * @param addr  Physical address of the frame (4 KiB aligned).
 */
void pmm_mark_free(uint64_t addr);

/**
 * @brief Query whether a physical frame is currently free.
 *
 * @param addr  Physical address (4 KiB aligned).
 * @return 1 if free, 0 if used/reserved, -1 if invalid address.
 */
int pmm_is_free(uint64_t addr);

/**
 * @brief Fill a pmm_stats_t with current allocator statistics.
 *
 * @param out  Destination struct.  Must not be NULL.
 */
void pmm_get_stats(pmm_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_PMM_H */
