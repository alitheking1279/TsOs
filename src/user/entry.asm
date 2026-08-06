; =============================================================================
; entry.asm — User-mode entry point (ring 3), the real _start of user_shell.elf
;
; Runs in ring 3.  The kernel's elf_load() maps this binary at 0x400000 and
; IRETQs into _start.  This stub:
;   1. Sets RSP to the top of the user stack
;   2. Calls the C entry point (user_task_entry in user_main.c)
;   3. On return, invokes SYS_EXIT (should never be reached)
;
; TASK_USER_STACK_BASE = 0x4000000000, TASK_USER_STACK_SIZE = 0x10000.
; =============================================================================

bits 64
default rel

%define SYS_EXIT    2

section .text

extern user_task_entry

global _start
_start:
    ; Set up user stack (top of the 64 KiB user stack region).
    mov rsp, 0x0000004000000000 + 0x10000

    ; Call the C entry point (user_task_entry in user_main.c).
    call user_task_entry

    ; Exit via syscall (should not reach here).
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

section .note.GNU-stack noalloc noexec nowrite progbits
