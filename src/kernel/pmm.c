/**
 * @file pmm.c
 * @brief Physical Memory Manager — bitmap-based 4 KiB frame allocator.
 *
 * Implementation notes
 * ====================
 *
 * Bitmap encoding
 * ---------------
 * g_bitmap[] is an array of PMM_BITMAP_WORDS × uint64_t.
 * Frame N occupies bit (N % 64) of word (N / 64).
 * Bit value 1 = USED, bit value 0 = FREE.
 *
 * All frames are initialised USED (0xFFFFFFFFFFFFFFFF) before the memory
 * map is parsed.  This means any frame not explicitly mentioned in the
 * Multiboot2 memory map is conservatively treated as reserved — correct for
 * MMIO holes, ACPI tables, firmware reservations, etc.
 *
 * Allocation
 * ----------
 * pmm_alloc_frame():
 *   Walk g_bitmap[] word by word.  If a word != UINT64_MAX (i.e. has at
 *   least one free bit), find the lowest free bit with __builtin_ctzll()
 *   (counts trailing zeros = index of lowest 0 when we pass ~word).
 *   Mark the bit used and return the physical address.  O(N/64).
 *
 * pmm_alloc_frames(count):
 *   Find the first run of `count` contiguous free frames using a sliding
 *   window.  O(N*count/64) — acceptable for small count at boot time.
 *
 * pmm_free_frame():
 *   Validate alignment and range, detect double-free, clear the bit. O(1).
 *
 * Serial logging
 * --------------
 * Every pmm_alloc_frame / pmm_free_frame call emits a one-line serial log
 * so that a debugging session in QEMU gives a full allocation trace without
 * any additional tooling.  Logging is gated through the g_serial pointer
 * so the PMM degrades gracefully if serial is not available.
 *
 * Protected regions (marked USED at init, never freed by the PMM):
 *   [0x00000000 … 0x00000FFF]  Frame 0   — BIOS IVT + BDA
 *   [_kernel_start … _kernel_end)  Kernel image
 *   [bitmap_start … bitmap_end)    PMM bitmap itself
 *
 * The Multiboot2 info structure is NOT reserved by the PMM.  If the caller
 * needs it after pmm_init(), they must either copy it first or reserve the
 * frames themselves.  This keeps pmm_init() minimal.
 */

#include "pmm.h"
#include "../drivers/serial.h"
#include <stdint.h>

/* =========================================================================
 * Linker-exported kernel image boundary symbols
 * Defined in linker.ld; addresses are page-aligned.
 * ========================================================================= */
extern char _kernel_start[];
extern char _kernel_end[];

/* =========================================================================
 * Internal bitmap storage
 *
 * Placed in .bss so the ELF loader initialises every bit to zero.
 * We immediately overwrite it in pmm_init() anyway, but this makes the
 * struct definition correct for C (tentative definitions need zero init).
 * ========================================================================= */
static uint64_t g_bitmap[PMM_BITMAP_WORDS];  /* 1-bit-per-frame, 1=USED */
static uint64_t g_total_frames;              /* highest frame index + 1   */
static uint64_t g_free_frames;
static uint64_t g_reserved_frames;
static int      g_initialized;               /* set to 1 after pmm_init() */

/* Serial device pointer for logging (may be NULL). */
static serial_dev_t *g_serial;

/* =========================================================================
 * Internal logging helpers
 *
 * Thin wrappers so the rest of the code can call pmm_log_*() without
 * checking g_serial every time.
 * ========================================================================= */

static void pmm_log_str(const char *s) {
    if (g_serial) { serial_write_string(g_serial, s); }
}

static void pmm_log_char(char c) {
    if (g_serial) { serial_write_char(g_serial, c); }
}

/** Print a 64-bit value as 0xHHHHHHHHHHHHHHHH to serial. */
static void pmm_log_hex64(uint64_t val) {
    static const char hex[] = "0123456789ABCDEF";
    if (!g_serial) return;
    serial_write_string(g_serial, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_write_char(g_serial, hex[(val >> i) & 0xF]);
    }
}

/** Print an unsigned decimal integer to serial. */
static void pmm_log_uint64(uint64_t val) {
    if (!g_serial) return;
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (val == 0) { serial_write_char(g_serial, '0'); return; }
    while (val > 0) { buf[--i] = (char)('0' + (val % 10)); val /= 10; }
    serial_write_string(g_serial, &buf[i]);
}

/* =========================================================================
 * Low-level bitmap operations (frame ↔ bit)
 * ========================================================================= */

/** Index of the bitmap word that contains frame `n`. */
static inline uint64_t _word_idx(uint64_t n) { return n / PMM_BITS_PER_WORD; }

/** Bit mask for frame `n` within its bitmap word. */
static inline uint64_t _bit_mask(uint64_t n) { return (uint64_t)1 << (n % PMM_BITS_PER_WORD); }

/** Return 1 if frame `n` is used (bit set), 0 if free. */
static inline int _is_used(uint64_t n) {
    return (g_bitmap[_word_idx(n)] & _bit_mask(n)) != 0;
}

/** Mark frame `n` as USED. */
static inline void _set_used(uint64_t n) {
    g_bitmap[_word_idx(n)] |= _bit_mask(n);
}

/** Mark frame `n` as FREE. */
static inline void _set_free(uint64_t n) {
    g_bitmap[_word_idx(n)] &= ~_bit_mask(n);
}

/* =========================================================================
 * Range helpers — operate in frame numbers
 * ========================================================================= */

/** Physical address → frame number (truncates to 4 KiB boundary). */
static inline uint64_t _addr_to_frame(uint64_t addr) {
    return addr >> PAGE_SHIFT;
}

/** Frame number → physical base address. */
static inline uint64_t _frame_to_addr(uint64_t frame) {
    return frame << PAGE_SHIFT;
}

/**
 * Mark all frames in [base_addr, base_addr + length) as FREE.
 * Clamps to PMM_MAX_FRAMES.  Only whole 4 KiB frames are freed:
 *   first frame = ceil(base_addr / PAGE_SIZE)
 *   last  frame = floor((base_addr + length) / PAGE_SIZE) - 1
 */
static void _mark_range_free(uint64_t base_addr, uint64_t length) {
    /* Round base up, end down — only free complete pages inside the region. */
    uint64_t first = (base_addr + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t end   = (base_addr + length) >> PAGE_SHIFT;

    if (end > PMM_MAX_FRAMES) end = PMM_MAX_FRAMES;
    if (first >= end) return;

    for (uint64_t f = first; f < end; f++) {
        if (_is_used(f)) {
            _set_free(f);
            g_free_frames++;
        }
    }
}

/**
 * Mark all frames in [base_addr, base_addr + length) as USED.
 * Clamps to PMM_MAX_FRAMES.
 */
static void _mark_range_used(uint64_t base_addr, uint64_t length) {
    uint64_t first = base_addr >> PAGE_SHIFT;
    uint64_t end   = ((base_addr + length + PAGE_SIZE - 1) >> PAGE_SHIFT);

    if (end > PMM_MAX_FRAMES) end = PMM_MAX_FRAMES;
    if (first >= end) return;

    for (uint64_t f = first; f < end; f++) {
        if (!_is_used(f)) {
            _set_used(f);
            g_free_frames--;
            g_reserved_frames++;
        }
    }
}

/* =========================================================================
 * Multiboot2 parsing helpers
 * ========================================================================= */

/** Align a pointer up to an 8-byte boundary (Multiboot2 tag alignment). */
static inline uint64_t _mb2_align8(uint64_t v) {
    return (v + 7ULL) & ~7ULL;
}

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

void pmm_init(uint32_t mb2_info_phys, void *serial) {
    g_serial = (serial_dev_t *)serial;

    pmm_log_str("\r\n[PMM] ============================================\r\n");
    pmm_log_str("[PMM] Physical Memory Manager initializing...\r\n");

    /* ---- Step 1: Mark every frame as USED (conservative start). ---- */
    for (uint64_t w = 0; w < PMM_BITMAP_WORDS; w++) {
        g_bitmap[w] = ~(uint64_t)0;   /* all bits = 1 = USED */
    }
    g_free_frames     = 0;
    g_reserved_frames = 0;
    g_total_frames    = PMM_MAX_FRAMES;

    /* ---- Step 2: Parse the Multiboot2 memory map. ---- */
    if (mb2_info_phys == 0) {
        pmm_log_str("[PMM] ERROR: NULL Multiboot2 info pointer — no memory freed.\r\n");
        g_initialized = 1;
        return;
    }

    uint64_t mb2_addr = (uint64_t)mb2_info_phys;
    mb2_header_t *hdr = (mb2_header_t *)mb2_addr;

    pmm_log_str("[PMM] Multiboot2 info @ ");
    pmm_log_hex64(mb2_addr);
    pmm_log_str(", total_size = ");
    pmm_log_uint64(hdr->total_size);
    pmm_log_str(" bytes\r\n");

    /* Walk every tag in the Multiboot2 info structure. */
    uint64_t offset   = sizeof(mb2_header_t);
    uint64_t mb2_size = (uint64_t)hdr->total_size;
    mb2_mmap_tag_t *mmap_tag = (mb2_mmap_tag_t *)0;  /* NULL until found */

    while (offset + sizeof(mb2_tag_t) <= mb2_size) {
        mb2_tag_t *tag = (mb2_tag_t *)(mb2_addr + offset);

        /* Tag type 0 = end tag. */
        if (tag->type == 0) break;

        if (tag->type == MB2_TAG_TYPE_MMAP) {
            mmap_tag = (mb2_mmap_tag_t *)(mb2_addr + offset);
        }

        /* Tags are 8-byte aligned. */
        offset += _mb2_align8((uint64_t)tag->size);
    }

    if (!mmap_tag) {
        pmm_log_str("[PMM] ERROR: No memory-map tag found in Multiboot2 info!\r\n");
        pmm_log_str("[PMM] Attempting fallback: freeing [1MiB, 4MiB).\r\n");
        _mark_range_free(0x100000ULL, 3ULL * 1024 * 1024);
        /* Fix 2: set g_total_frames to the top of the fallback region so
         * stats are accurate (not left at the unreachable PMM_MAX_FRAMES). */
        g_total_frames = (0x100000ULL + 3ULL * 1024 * 1024) >> PAGE_SHIFT;
    } else {
        pmm_log_str("[PMM] Memory map (entry_size=");
        pmm_log_uint64(mmap_tag->entry_size);
        pmm_log_str("):\r\n");

        /* Iterate over every memory-map entry. */
        uint64_t entries_start = (uint64_t)mmap_tag + sizeof(mb2_mmap_tag_t);
        uint64_t entries_end   = (uint64_t)mmap_tag + (uint64_t)mmap_tag->size;
        uint64_t entry_addr    = entries_start;

        uint64_t highest_usable = 0;

        while (entry_addr + mmap_tag->entry_size <= entries_end) {
            mb2_mmap_entry_t *e = (mb2_mmap_entry_t *)entry_addr;

            pmm_log_str("[PMM]   [");
            pmm_log_hex64(e->base_addr);
            pmm_log_str(" - ");
            pmm_log_hex64(e->base_addr + e->length);
            pmm_log_str(") type=");
            pmm_log_uint64(e->type);
            pmm_log_str(" (");
            if (e->type == MB2_MMAP_AVAILABLE) {
                pmm_log_str("AVAILABLE");
                _mark_range_free(e->base_addr, e->length);
                uint64_t end = e->base_addr + e->length;
                if (end > highest_usable) highest_usable = end;
            } else {
                pmm_log_str("RESERVED ");
            }
            pmm_log_str(")\r\n");

            entry_addr += mmap_tag->entry_size;
        }

        /* Track how many frames are actually meaningful. */
        uint64_t top_frame = (highest_usable + PAGE_SIZE - 1) >> PAGE_SHIFT;
        if (top_frame > PMM_MAX_FRAMES) top_frame = PMM_MAX_FRAMES;
        g_total_frames = top_frame;
    }

    /* ---- Step 3: Re-reserve frame 0 (BIOS IVT + BDA). ---- */
    if (!_is_used(0)) {
        _set_used(0);
        g_free_frames--;
        g_reserved_frames++;
    }
    pmm_log_str("[PMM] Frame 0 reserved (BIOS IVT/BDA).\r\n");

    /* ---- Step 4: Re-reserve the kernel image. ---- */
    uint64_t ks = (uint64_t)(uintptr_t)_kernel_start;
    uint64_t ke = (uint64_t)(uintptr_t)_kernel_end;
    pmm_log_str("[PMM] Kernel image: ");
    pmm_log_hex64(ks);
    pmm_log_str(" - ");
    pmm_log_hex64(ke);
    pmm_log_str("\r\n");
    _mark_range_used(ks, ke - ks);
    pmm_log_str("[PMM] Kernel image reserved.\r\n");

    /* ---- Step 5: Re-reserve the PMM bitmap itself. ---- */
    uint64_t bmap_start = (uint64_t)(uintptr_t)g_bitmap;
    uint64_t bmap_size  = (uint64_t)sizeof(g_bitmap);
    pmm_log_str("[PMM] Bitmap: ");
    pmm_log_hex64(bmap_start);
    pmm_log_str(" - ");
    pmm_log_hex64(bmap_start + bmap_size);
    pmm_log_str(" (");
    pmm_log_uint64(bmap_size / 1024);
    pmm_log_str(" KiB)\r\n");
    _mark_range_used(bmap_start, bmap_size);
    pmm_log_str("[PMM] Bitmap reserved.\r\n");

    g_initialized = 1;

    pmm_log_str("[PMM] ============================================\r\n");
    pmm_log_str("[PMM] Init complete:\r\n");
    pmm_log_str("[PMM]   Total usable frames : "); pmm_log_uint64(g_total_frames);   pmm_log_str("\r\n");
    pmm_log_str("[PMM]   Free  frames        : "); pmm_log_uint64(g_free_frames);    pmm_log_str("\r\n");
    pmm_log_str("[PMM]   Used/reserved       : "); pmm_log_uint64(g_total_frames - g_free_frames); pmm_log_str("\r\n");
    pmm_log_str("[PMM]   Free RAM            : "); pmm_log_uint64(g_free_frames * 4); pmm_log_str(" KiB\r\n");
    pmm_log_str("[PMM] ============================================\r\n");
}

uint64_t pmm_alloc_frame(void) {
    if (!g_initialized) return 0;

    uint64_t n_words = g_total_frames / PMM_BITS_PER_WORD;
    if (n_words == 0) n_words = 1;

    for (uint64_t w = 0; w < n_words; w++) {
        if (g_bitmap[w] == ~(uint64_t)0) continue;  /* all used — fast skip */

        /* At least one bit is 0 (free). Find lowest free bit. */
        uint64_t free_bits = ~g_bitmap[w];           /* 1 = free now */

        /* Fix 3: Frame 0 (bit 0 of word 0) must NEVER be allocated.
         * Mask it out structurally so __builtin_ctzll can never pick it,
         * even if the bitmap bit were somehow cleared by a bug. */
        if (w == 0) free_bits &= ~(uint64_t)1;
        if (free_bits == 0) continue;                /* only frame 0 was free */

        int bit = __builtin_ctzll(free_bits);        /* index of lowest set bit */
        uint64_t frame = w * PMM_BITS_PER_WORD + (uint64_t)bit;

        if (frame >= g_total_frames) return 0;       /* out of range */

        _set_used(frame);
        g_free_frames--;

        uint64_t addr = _frame_to_addr(frame);
        pmm_log_str("[PMM] ALLOC frame ");
        pmm_log_uint64(frame);
        pmm_log_str(" @ ");
        pmm_log_hex64(addr);
        pmm_log_str("\r\n");
        return addr;
    }

    pmm_log_str("[PMM] ALLOC failed: OOM\r\n");
    return 0;  /* OOM */
}

uint64_t pmm_alloc_frames(uint64_t count) {
    if (!g_initialized || count == 0) return 0;
    if (count == 1) return pmm_alloc_frame();

    /* Find first run of `count` contiguous free frames. */
    uint64_t run_start = 0;
    uint64_t run_len   = 0;

    for (uint64_t f = 1; f < g_total_frames; f++) {
        if (!_is_used(f)) {
            if (run_len == 0) run_start = f;
            run_len++;
            if (run_len == count) {
                /* Found a sufficient run — allocate it. */
                for (uint64_t i = run_start; i < run_start + count; i++) {
                    _set_used(i);
                    g_free_frames--;
                }
                uint64_t addr = _frame_to_addr(run_start);
                pmm_log_str("[PMM] ALLOC_CONTIG ");
                pmm_log_uint64(count);
                pmm_log_str(" frames [");
                pmm_log_uint64(run_start);
                pmm_log_str("] @ ");
                pmm_log_hex64(addr);
                pmm_log_str("\r\n");
                return addr;
            }
        } else {
            run_len = 0;
        }
    }

    pmm_log_str("[PMM] ALLOC_CONTIG failed: no ");
    pmm_log_uint64(count);
    pmm_log_str(" contiguous frames available\r\n");
    return 0;
}

pmm_status_t pmm_free_frame(uint64_t addr) {
    if (!g_initialized)           return PMM_ERR_NOT_INIT;
    if (addr & (PAGE_SIZE - 1))   return PMM_ERR_INVALID;   /* misaligned */

    uint64_t frame = _addr_to_frame(addr);
    if (frame == 0 || frame >= PMM_MAX_FRAMES) return PMM_ERR_INVALID;

    if (!_is_used(frame)) {
        pmm_log_str("[PMM] DOUBLE-FREE detected @ ");
        pmm_log_hex64(addr);
        pmm_log_str("\r\n");
        return PMM_ERR_ALREADY_FREE;
    }

    _set_free(frame);
    g_free_frames++;

    pmm_log_str("[PMM] FREE  frame ");
    pmm_log_uint64(frame);
    pmm_log_str(" @ ");
    pmm_log_hex64(addr);
    pmm_log_str("\r\n");
    return PMM_OK;
}

void pmm_mark_used(uint64_t addr) {
    if (addr & (PAGE_SIZE - 1)) return;
    uint64_t frame = _addr_to_frame(addr);
    if (frame >= PMM_MAX_FRAMES) return;
    if (!_is_used(frame)) {
        _set_used(frame);
        g_free_frames--;
    }
}

void pmm_mark_free(uint64_t addr) {
    if (addr & (PAGE_SIZE - 1)) return;
    uint64_t frame = _addr_to_frame(addr);
    /* Fix 1: Frame 0 is permanently reserved (BIOS IVT/BDA).
     * Silently ignore any attempt to free it — this is a hard invariant
     * that must hold regardless of how the caller obtained `addr`. */
    if (frame == 0) return;
    if (frame >= PMM_MAX_FRAMES) return;
    if (_is_used(frame)) {
        _set_free(frame);
        g_free_frames++;
    }
}

int pmm_is_free(uint64_t addr) {
    if (addr & (PAGE_SIZE - 1)) return -1;
    uint64_t frame = _addr_to_frame(addr);
    if (frame >= PMM_MAX_FRAMES) return -1;
    return _is_used(frame) ? 0 : 1;
}

void pmm_get_stats(pmm_stats_t *out) {
    if (!out) return;
    out->total_frames    = g_total_frames;
    out->free_frames     = g_free_frames;
    out->used_frames     = g_total_frames - g_free_frames;
    out->reserved_frames = g_reserved_frames;
}
