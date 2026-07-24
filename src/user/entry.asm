; =============================================================================
; entry.asm — User-mode entry point (ring 3)
;
; This file is NOT compiled into the kernel.  It exists as a reference
; for what a real user-mode binary would look like.  For testing, the
; kernel maps user_task_entry (from user_main.c) at 0x400000.
;
; A real user binary would:
;   1. Set RSP to the user stack top
;   2. Call main or _start
;   3. On return, invoke SYS_EXIT
;
; This file serves as documentation of the user-side ABI.
; =============================================================================

bits 64
default rel

%define SYS_EXIT    2
%define SYS_WRITE   0

section .text

global _start
_start:
    ; Set up user stack
    mov rsp, 0x0000004000000000 + 0x10000   ; TASK_USER_STACK_BASE + TASK_USER_STACK_SIZE

    ; Call the C entry point (user_task_entry in user_main.c)
    call user_task_entry

    ; Exit via syscall (should not reach here)
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall
