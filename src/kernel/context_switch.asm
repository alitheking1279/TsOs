; =============================================================================
; context_switch.asm — Save/restore callee-saved registers and swap kernel stacks.
;
; This is the heart of preemptive multitasking. When the scheduler decides
; to switch from task A to task B:
;   1. Timer IRQ fires → CPU is on task A's kernel stack.
;   2. ISR stub + isr_common_handler + timer_handler run on task A's stack.
;   3. scheduler_tick() calls context_switch(prev=A, next=B).
;   4. context_switch saves A's callee-saved regs, swaps RSP, restores B's.
;   5. context_switch returns → we're now executing on task B's stack.
;   6. The call chain unwinds back through scheduler_tick → timer_handler
;      → isr_common_handler → iretq, restoring task B's full CPU state.
;
; Calling convention: System V AMD64 ABI.
;   rdi = prev task_t* (pointer to the outgoing task)
;   rsi = next task_t* (pointer to the incoming task)
;
; Callee-saved registers (saved/restored): rbx, rbp, r12-r15
; Caller-saved registers (not saved — already in interrupt frame): rax, rcx, rdx, rsi, rdi, r8-r11
;
; Stack layout for a NEW task (set up by task_create):
;   kernel_rsp points to →
;     [+0]  entry function address  (what RET will jump to)
;     [+8]  dummy rbx = 0
;     [+16] dummy rbp = 0
;     [+24] dummy r12 = 0
;     [+32] dummy r13 = 0
;     [+40] dummy r14 = 0
;     [+48] dummy r15 = 0       ← RSP points here before the pops
;
; For a task that has been switched out before:
;   kernel_rsp points to →
;     [+0]  saved RIP (return address from when context_switch was called)
;     [+8]  saved rbx
;     [+16] saved rbp
;     [+24] saved r12
;     [+32] saved r13
;     [+40] saved r14
;     [+48] saved r15          ← RSP points here before the pops
; =============================================================================

bits 64
section .text

; task_t field offsets (must match task.h layout)
%define TASK_KERNEL_RSP_OFFSET   0       ; offset of kernel_rsp field

; task_t fields that context_switch accesses:
;   offset 0:  kernel_rsp   (uint64_t)
;   offset 8:  rbp          (uint64_t)
;   offset 16: rbx          (uint64_t)
;   offset 24: r12          (uint64_t)
;   offset 32: r13          (uint64_t)
;   offset 40: r14          (uint64_t)
;   offset 48: r15          (uint64_t)

global context_switch
context_switch:
    ; ------------------------------------------------------------------
    ; SAVE prev task's context
    ; ------------------------------------------------------------------

    ; Push callee-saved registers onto prev's kernel stack.
    ; Order matches the reverse of the pop order below.
    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx

    ; Save prev's RSP (which now points to the saved register block)
    ; into prev->kernel_rsp.
    mov [rdi + TASK_KERNEL_RSP_OFFSET], rsp
    ; rdi = prev, store current RSP at offset 0 (kernel_rsp)

    ; ------------------------------------------------------------------
    ; LOAD next task's context
    ; ------------------------------------------------------------------

    ; Load next's kernel RSP from next->kernel_rsp.
    mov rsp, [rsi + TASK_KERNEL_RSP_OFFSET]

    ; Restore callee-saved registers from next's kernel stack.
    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r14
    pop r15

    ; Return to wherever next was last switched out (or to its entry point).
    ret
