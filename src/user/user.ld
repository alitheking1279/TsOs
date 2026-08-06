/**
 * user.ld — Linker script for the ring-3 user program (user_shell.elf).
 *
 * The ELF is loaded by the kernel's elf_load() (src/kernel/elf.c):
 *   - must be ELF64 ET_EXEC with PT_LOAD segments in the user half
 *   - entry point must be < 0x800000000000
 *   - text must be R+X, data R+W (data becomes NX via PF_X absence)
 *
 * Linked at 0x400000, well below the user stack at TASK_USER_STACK_BASE
 * (0x4000000000) and the user heap (starts above the highest segment).
 */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

PHDRS
{
    text    PT_LOAD FLAGS(5);   /* R+X */
    data    PT_LOAD FLAGS(6);   /* R+W */
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text*)
    } :text

    .rodata : {
        *(.rodata*)
    } :data

    .data : {
        *(.data*)
    } :data

    .bss : {
        *(.bss*)
        *(COMMON)
    } :data
}
