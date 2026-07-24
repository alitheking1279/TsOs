/**
 * @file elf.h
 * @brief ELF64 header and program header definitions for loading executables.
 *
 * Defines the ELF64 file format structures needed to load a static
 * executable into a user address space.  Only the subset required for
 * loading (ET_EXEC, PT_LOAD) is implemented.
 *
 * References:
 *   System V ABI — ELF64 Specification
 *   https://refspecs.linuxfoundation.org/elf/elf.pdf
 */

#ifndef KERNEL_ELF_H
#define KERNEL_ELF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "page_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * ELF64 Magic and Identification
 * ========================================================================= */

/** ELF magic bytes: 0x7f 'E' 'L' 'F'. */
#define EI_MAGIC0   0x7F
#define EI_MAGIC1   'E'
#define EI_MAGIC2   'L'
#define EI_MAGIC3   'F'

/** ELF class identifiers (e_ident[EI_CLASS]). */
#define ELFCLASS_NONE   0
#define ELFCLASS_32     1
#define ELFCLASS_64     2

/** ELF data encoding (e_ident[EI_DATA]). */
#define ELFDATA_NONE    0
#define ELFDATA_LSB     1   /**< Little-endian. */
#define ELFDATA_MSB     2   /**< Big-endian. */

/** ELF OS/ABI (e_ident[EI_OSABI]). */
#define ELFOSABI_NONE       0
#define ELFOSABI_LINUX      3

/** e_ident indices. */
#define EI_MAG0         0
#define EI_MAG1         1
#define EI_MAG2         2
#define EI_MAG3         3
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_NIDENT       16

/* =========================================================================
 * ELF64 File Header
 *
 * The ELF header is at the very beginning of the file.  It identifies
 * the file as an ELF and tells us where to find program headers
 * (which describe how to load segments into memory).
 * ========================================================================= */

typedef struct {
    uint8_t     e_ident[EI_NIDENT]; /**< ELF identification (16 bytes). */
    uint16_t    e_type;             /**< Object file type (ET_EXEC = 2). */
    uint16_t    e_machine;          /**< Architecture (EM_X86_64 = 0x3E). */
    uint32_t    e_version;          /**< Object file version (1). */
    uint64_t    e_entry;            /**< Entry point virtual address. */
    uint64_t    e_phoff;            /**< Program header table offset (bytes). */
    uint64_t    e_shoff;            /**< Section header table offset (bytes). */
    uint32_t    e_flags;            /**< Processor-specific flags. */
    uint16_t    e_ehsize;           /**< ELF header size (bytes). */
    uint16_t    e_phentsize;        /**< Program header entry size (bytes). */
    uint16_t    e_phnum;            /**< Number of program header entries. */
    uint16_t    e_shentsize;        /**< Section header entry size (bytes). */
    uint16_t    e_shnum;            /**< Number of section header entries. */
    uint16_t    e_shstrndx;         /**< Section name string table index. */
} __attribute__((packed)) elf64_header_t;

/* ELF type constants (e_type). */
#define ET_NONE     0
#define ET_REL      1
#define ET_EXEC     2   /**< Executable file. */
#define ET_DYN      3   /**< Shared object / PIE. */
#define ET_CORE     4

/* ELF machine constants (e_machine). */
#define EM_X86_64   0x3E

/* =========================================================================
 * ELF64 Program Header (Segment Descriptor)
 *
 * Each program header describes a segment to load into memory.
 * For loading an executable, we only care about PT_LOAD segments.
 * ========================================================================= */

typedef struct {
    uint32_t    p_type;     /**< Segment type. */
    uint32_t    p_flags;    /**< Segment flags (PF_R, PF_W, PF_X). */
    uint64_t    p_offset;   /**< Offset in the file (bytes). */
    uint64_t    p_vaddr;    /**< Virtual address of segment in memory. */
    uint64_t    p_paddr;    /**< Physical address (unused for user loading). */
    uint64_t    p_filesz;   /**< Size of segment in the file (bytes). */
    uint64_t    p_memsz;    /**< Size of segment in memory (bytes, >= p_filesz). */
    uint64_t    p_align;    /**< Alignment (bytes, typically 0x1000 or 0x200000). */
} __attribute__((packed)) elf64_phdr_t;

/* Program header type constants (p_type). */
#define PT_NULL     0
#define PT_LOAD     1   /**< Loadable segment. */
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_PHDR     6
#define PT_TLS      7

/* Program header flag bits (p_flags). */
#define PF_X        0x1     /**< Execute. */
#define PF_W        0x2     /**< Write. */
#define PF_R        0x4     /**< Read. */

/* =========================================================================
 * ELF64 Return Codes
 * ========================================================================= */

typedef enum {
    ELF_OK              =  0,  /**< Success. */
    ELF_ERR_INVALID     = -1,  /**< Bad ELF magic, class, or header. */
    ELF_ERR_NOT_EXEC    = -2,  /**< Not an executable (e_type != ET_EXEC/ET_DYN). */
    ELF_ERR_NOT_X86_64  = -3,  /**< Wrong architecture (e_machine != EM_X86_64). */
    ELF_ERR_TOO_MANY    = -4,  /**< Too many program headers to load. */
    ELF_ERR_NO_LOAD     = -5,  /**< No PT_LOAD segments found. */
    ELF_ERR_NO_MEM      = -6,  /**< Physical memory allocation failed. */
    ELF_ERR_MAP_FAILED  = -7,  /**< Virtual memory mapping failed. */
    ELF_ERR_BAD_OFFSET  = -8,  /**< Segment offset exceeds file size. */
    ELF_ERR_BAD_VADDR   = -9,  /**< Segment vaddr is in kernel half. */
} elf_status_t;

/* =========================================================================
 * Maximum Limits
 * ========================================================================= */

/** Maximum number of program headers we will process. */
#define ELF_MAX_PHDRS   16

/** Maximum total size of all loaded segments (256 MiB). */
#define ELF_MAX_LOAD_SIZE   (256UL * 1024 * 1024)

/* =========================================================================
 * ELF Loading Result
 * ========================================================================= */

/** Information about a successfully loaded ELF image. */
typedef struct {
    uint64_t    entry;      /**< Entry point virtual address. */
    uint64_t    base;       /**< Lowest loaded segment address (page-aligned). */
    uint64_t    top;        /**< Highest loaded segment end address (page-aligned). */
    uint32_t    phdr_count; /**< Number of PT_LOAD segments loaded. */
} elf_load_result_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the ELF loader subsystem.
 *
 * @param serial_dev  Serial device for logging. May be NULL.
 */
void elf_init(void *serial_dev);

/**
 * @brief Validate an ELF64 header.
 *
 * Checks magic bytes, class (ELFCLASS64), data encoding (little-endian),
 * type (ET_EXEC or ET_DYN), and machine (EM_X86_64).
 *
 * @param data      Pointer to the start of the ELF data.
 * @param size      Total size of the ELF data in bytes.
 * @param[out] hdr  Populated with the validated ELF header.
 * @return ELF_OK on success, or an error code.
 */
elf_status_t elf_validate(const void *data, size_t size, elf64_header_t *hdr);

/**
 * @brief Load an ELF64 executable into an address space.
 *
 * Parses the ELF headers, iterates PT_LOAD segments, maps them into the
 * target address space with DEMAND PTEs (lazy loading), and populates
 * the result structure.
 *
 * For each PT_LOAD segment:
 *   - Validates offset+filesz don't exceed file size
 *   - Validates vaddr is in user half
 *   - Creates DEMAND PTEs (present=0, writable=1, PTE_DEMAND set)
 *     for every page in the segment range [vaddr, vaddr+memsz)
 *   - Copies file data into freshly allocated frames on first access
 *
 * The entry point is returned in result->entry for use by the caller
 * to set up the user RIP.
 *
 * @param data      Pointer to the start of the ELF data.
 * @param size      Total size of the ELF data in bytes.
 * @param as        Target address space (must not be the kernel address space).
 * @param[out] result  Populated with load information.
 * @return ELF_OK on success, or an error code.
 */
elf_status_t elf_load(const void *data, size_t size,
                      address_space_t *as, elf_load_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_ELF_H */
