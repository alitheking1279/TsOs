ENTRY(_start)

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    .text :
    {
        KEEP(*(.multiboot))
        *(.text)
    }

    .rodata : { *(.rodata) }
    .data   : { *(.data) }

    .bss :
    {
        *(.bss)
        *(COMMON)
    }

    /* Boot stack lives in .bootstrap_stack (nobits, like .bss). */
    .bootstrap_stack (NOLOAD) :
    {
        *(.bootstrap_stack)
    }

    /* Page-align the end marker so the PMM can use it as a frame boundary. */
    . = ALIGN(4096);
    _kernel_end = .;
}