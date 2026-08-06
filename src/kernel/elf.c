/**
 * @file elf.c
 * @brief ELF64 loader — validates and maps PT_LOAD segments into an address space.
 *
 * Loads a statically-linked ELF64 executable into a user address space
 * using eager frame allocation.  Each PT_LOAD segment's pages are
 * allocated, zeroed, file data copied, and mapped with user permissions.
 * Demand paging is used for pages beyond p_filesz (BSS zero-fill).
 *
 * References:
 *   System V ABI — ELF64 Specification
 *   https://refspecs.linuxfoundation.org/elf/elf.pdf
 */

#include "elf.h"
#include "page_table.h"
#include "vmm.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include <string.h>

/* =========================================================================
 * Serial logging
 * ========================================================================= */

static serial_dev_t *g_elf_serial = NULL;

static void elf_log(const char *msg) {
    if (g_elf_serial) serial_write_string(g_elf_serial, msg);
}

static void elf_log_hex(uint64_t val) {
    if (!g_elf_serial) return;
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[18] = '\0';
    serial_write_string(g_elf_serial, buf);
}

/* =========================================================================
 * Initialization
 * ========================================================================= */

void elf_init(void *serial_dev) {
    g_elf_serial = (serial_dev_t *)serial_dev;
    elf_log("[ELF] ELF subsystem initialized.\r\n");
}

/* =========================================================================
 * Validation
 * ========================================================================= */

elf_status_t elf_validate(const void *data, size_t size, elf64_header_t *hdr) {
    if (!data || size < sizeof(elf64_header_t)) {
        return ELF_ERR_INVALID;
    }

    /* Copy header to local for safe access. */
    memcpy(hdr, data, sizeof(elf64_header_t));

    /* Check magic bytes. */
    if (hdr->e_ident[0] != EI_MAGIC0 || hdr->e_ident[1] != EI_MAGIC1 ||
        hdr->e_ident[2] != EI_MAGIC2 || hdr->e_ident[3] != EI_MAGIC3) {
        elf_log("[ELF] ERR: bad magic\r\n");
        return ELF_ERR_INVALID;
    }

    /* Must be ELFCLASS64. */
    if (hdr->e_ident[EI_CLASS] != ELFCLASS_64) {
        elf_log("[ELF] ERR: not ELF64\r\n");
        return ELF_ERR_INVALID;
    }

    /* Must be little-endian. */
    if (hdr->e_ident[EI_DATA] != ELFDATA_LSB) {
        elf_log("[ELF] ERR: not little-endian\r\n");
        return ELF_ERR_INVALID;
    }

    /* Must be an executable (or shared object for PIE). */
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        elf_log("[ELF] ERR: not executable\r\n");
        return ELF_ERR_NOT_EXEC;
    }

    /* Must be x86-64. */
    if (hdr->e_machine != EM_X86_64) {
        elf_log("[ELF] ERR: not x86-64\r\n");
        return ELF_ERR_NOT_X86_64;
    }

    /* Validate program header table fits within file. */
    if (hdr->e_phoff + (uint64_t)hdr->e_phentsize * hdr->e_phnum > size) {
        elf_log("[ELF] ERR: phdr table exceeds file size\r\n");
        return ELF_ERR_INVALID;
    }

    /* Validate entry point is in user half. */
    if (hdr->e_entry != 0 && hdr->e_entry >= 0x800000000000ULL) {
        elf_log("[ELF] ERR: entry in kernel half\r\n");
        return ELF_ERR_BAD_VADDR;
    }

    elf_log("[ELF] Validation OK — entry=");
    elf_log_hex(hdr->e_entry);
    elf_log(" phnum=");
    {
        char buf[6];
        int n = hdr->e_phnum;
        int i = 0;
        if (n == 0) { buf[i++] = '0'; }
        else {
            char tmp[6]; int j = 0;
            while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
            while (j > 0) { buf[i++] = tmp[--j]; }
        }
        buf[i] = '\0';
        elf_log(buf);
    }
    elf_log("\r\n");

    return ELF_OK;
}

/* =========================================================================
 * ELF Loading
 * ========================================================================= */

elf_status_t elf_load(const void *data, size_t size,
                      address_space_t *as, elf_load_result_t *result) {
    if (!data || !as || !result) return ELF_ERR_INVALID;

    elf64_header_t hdr;
    elf_status_t st = elf_validate(data, size, &hdr);
    if (st != ELF_OK) return st;

    /* Initialize result. */
    result->entry = hdr.e_entry;
    result->base = 0xFFFFFFFFFFFFFFFFULL;  /* Will be overwritten */
    result->top = 0;
    result->phdr_count = 0;

    elf_log("[ELF] Loading segments...\r\n");

    /* Iterate program headers. */
    const uint8_t *file = (const uint8_t *)data;
    const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(file + hdr.e_phoff);

    for (uint16_t i = 0; i < hdr.e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];

        /* Only load PT_LOAD segments. */
        if (ph->p_type != PT_LOAD) continue;

        /* Validate segment bounds. */
        if (ph->p_offset + ph->p_filesz > size) {
            elf_log("[ELF] ERR: segment exceeds file\r\n");
            return ELF_ERR_BAD_OFFSET;
        }

        /* Segment must be in user half. */
        if (ph->p_vaddr < 0x1000 ||
            ph->p_vaddr + ph->p_memsz > 0x800000000000ULL) {
            elf_log("[ELF] ERR: segment vaddr in kernel half\r\n");
            return ELF_ERR_BAD_VADDR;
        }

        /* Compute page-aligned range. */
        uint64_t vstart = ph->p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t vend = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1)
                       & ~(PAGE_SIZE - 1);

        /* Check total load size. */
        if (vend - vstart > ELF_MAX_LOAD_SIZE) {
            elf_log("[ELF] ERR: segment too large\r\n");
            return ELF_ERR_TOO_MANY;
        }

        /* Track base/top. */
        if (vstart < result->base) result->base = vstart;
        if (vend > result->top) result->top = vend;

        /* Build PTE flags from segment permissions. */
        uint64_t flags = PTE_USER;
        if (ph->p_flags & PF_R) flags |= PTE_PRESENT;  /* readable */
        if (ph->p_flags & PF_W) flags |= PTE_WRITABLE;
        if (!(ph->p_flags & PF_X)) flags |= PTE_NX;    /* NX if not executable */

        /* Copy the file bytes that overlap the page [va, va+PAGE_SIZE).
         * Handles the first page of a segment whose start is not
         * page-aligned (the page may already be mapped by the previous
         * segment and must only receive its own bytes). */
        void copy_page_overlap(void *frame_virt, uint64_t va) {
            uint64_t seg_end = ph->p_vaddr + ph->p_filesz;
            uint64_t page_end = va + PAGE_SIZE;
            uint64_t lo = (va > ph->p_vaddr) ? va : ph->p_vaddr;
            uint64_t hi = (page_end < seg_end) ? page_end : seg_end;
            if (lo >= hi) return;
            uint64_t copy_len = hi - lo;
            uint64_t dest_off = lo - va;
            uint64_t src_off = ph->p_offset + (lo - ph->p_vaddr);
            memcpy((uint8_t *)frame_virt + dest_off,
                   file + src_off, copy_len);
        }

        elf_log("[ELF] PT_LOAD @ vaddr=");
        elf_log_hex(ph->p_vaddr);
        elf_log(" filesz=");
        elf_log_hex(ph->p_filesz);
        elf_log(" memsz=");
        elf_log_hex(ph->p_memsz);
        elf_log("\r\n");

        /* Map each page in this segment. */
        for (uint64_t va = vstart; va < vend; va += PAGE_SIZE) {
            /* If the page is already mapped (a previous PT_LOAD covers
             * this page too — happens when segments share a page), write
             * this segment's bytes into the existing frame and upgrade
             * permissions instead of mapping a fresh frame. */
            uint64_t existing = vmm_translate(as, va);

            if (existing != 0) {
                /* Existing frame: copy this segment's bytes on top. */
                void *frame_virt = pt_phys_to_virt(existing);
                copy_page_overlap(frame_virt, va);

                vmm_status_t mst = vmm_map_page_merge(as, va, existing, flags);
                if (mst != VMM_OK) {
                    elf_log("[ELF] ERR: vmm_map_page_merge failed\r\n");
                    return ELF_ERR_MAP_FAILED;
                }
                continue;
            }

            /* Allocate a physical frame. */
            uint64_t frame = pmm_alloc_frame();
            if (frame == 0) {
                elf_log("[ELF] ERR: OOM mapping segment\r\n");
                return ELF_ERR_NO_MEM;
            }

            /* Zero the frame (handles BSS zero-fill). */
            void *frame_virt = pt_phys_to_virt(frame);
            memset(frame_virt, 0, PAGE_SIZE);

            /* Copy file data into this page if it overlaps p_filesz. */
            copy_page_overlap(frame_virt, va);

            /* Map into address space. */
            vmm_status_t mst = vmm_map_page_merge(as, va, frame, flags);
            if (mst != VMM_OK) {
                elf_log("[ELF] ERR: vmm_map_page failed\r\n");
                return ELF_ERR_MAP_FAILED;
            }
        }

        result->phdr_count++;
    }

    if (result->phdr_count == 0) {
        elf_log("[ELF] ERR: no PT_LOAD segments\r\n");
        return ELF_ERR_NO_LOAD;
    }

    elf_log("[ELF] Load complete — entry=");
    elf_log_hex(result->entry);
    elf_log(" base=");
    elf_log_hex(result->base);
    elf_log(" top=");
    elf_log_hex(result->top);
    elf_log("\r\n");

    return ELF_OK;
}
