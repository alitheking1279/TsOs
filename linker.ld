ENTRY(_start)

KERNEL_OFFSET = 0xFFFFFFFF80000000;

SECTIONS
{
    /* =================================================================
     * Boot code and data — runs at physical addresses during 32-bit init.
     *
     * Placed at physical 1M (no offset) so the32-bit boot code can
     * address everything with plain 32-bit pointers before paging is
     * enabled.  The multiboot header must be in the first 32 KiB of
     * the loaded image, which it is since everything starts at 1M.
     * ================================================================= */
    . = 1M;

    .boot.text : {
        *(.boot.text)
    }

    .boot.data : {
        *(.boot.data)
    }

    /* =================================================================
     * Kernel code and data — higher-half virtual addresses.
     *
     * VMA  = KERNEL_OFFSET + current location counter
     *        (= KERNEL_OFFSET + 1M + boot_size at this point)
     * LMA  = VMA - KERNEL_OFFSET  (physical load address)
     *
     * Since VMA - KERNEL_OFFSET == LMA, the simple boot page tables
     * (PA = VA - KERNEL_OFFSET) map kernel code correctly.
     *
     * _kernel_start / _kernel_end are virtual addresses; the PMM
     * subtracts HIGHER_HALF_OFFSET to get the physical range.
     * ================================================================= */
    . = KERNEL_OFFSET + .;

    _kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_OFFSET) {
        *(.text)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_OFFSET) { *(.rodata) }
    .data : AT(ADDR(.data) - KERNEL_OFFSET) {
        *(.data)
        *(.data.rel.ro*)
    }

    .bss : AT(ADDR(.bss) - KERNEL_OFFSET) {
        *(.bss)
        *(COMMON)
    }

    /* Bootstrap stack for kernel_main (higher-half, NOLOAD). */
    .bootstrap_stack (NOLOAD) : {
        *(.bootstrap_stack)
    }

    /* Page-align the end marker so the PMM can use it as a frame boundary. */
    . = ALIGN(4096);
    _kernel_end = .;
}
