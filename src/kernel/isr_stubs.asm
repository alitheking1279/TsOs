; =============================================================================
; isr_stubs.asm — 256 ISR stubs + shared common tail + stub address table
;
; Each CPU exception and hardware IRQ has a dedicated stub.  The stub
; pushes the vector number and (for exceptions without an error code)
; a dummy error_code of 0, then jumps to the shared isr_common tail.
;
; The shared tail saves all 16 GP registers, calls the C dispatcher
; isr_common_handler(interrupt_frame_t *), restores registers, cleans
; up the frame, and executes iretq.
;
; Error-code exception vectors:
;   8 (Double Fault), 10 (Invalid TSS), 11 (Segment Not Present),
;   12 (Stack-Segment Fault), 13 (General Protection),
;   14 (Page Fault), 17 (Alignment Check), 21 (Machine Check),
;   29 (VMM Communication), 30 (Security Exception)
;
; All other vectors use the no-error-code path.
;
; Calling convention: System V AMD64 ABI throughout.
; =============================================================================

bits 64
section .text

extern isr_common_handler

; =============================================================================
; Stub macros
;
; NOERRCODE: The CPU did NOT push an error code.  We push a dummy 0 as
;            error_code, then the vector number.  This produces the same
;            stack layout as the ERRCODE path.
;
; ERRCODE:   The CPU DID push an error code.  We push only the vector
;            number above it.  After the common tail pushes GP registers,
;            the frame layout is identical to the NOERRCODE path.
; =============================================================================

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0        ; dummy error_code (symmetry with ERRCODE path)
    push qword %1       ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    ; error_code already on stack from CPU push
    push qword %1       ; vector number (above the error_code)
    jmp isr_common
%endmacro

; =============================================================================
; Shared common tail
;
; Stack on entry to isr_common (after stub macro):
;   [rsp+0]  vector         (pushed by stub)
;   [rsp+8]  error_code     (real or dummy)
;   [rsp+16] rip            (from CPU)
;   [rsp+24] cs             (from CPU)
;   [rsp+32] rflags         (from CPU)
;   [rsp+40] rsp            (from CPU, only on CPL change)
;   [rsp+48] ss             (from CPU, only on CPL change)
;
; After GP register pushes, rsp points to the interrupt_frame_t struct
; which is passed as the first argument (rdi) to isr_common_handler.
; =============================================================================

isr_common:
    swapgs                          ; Switch to kernel GS-base on entry
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld                         ; Clear direction flag — SDM requires DF=0 for string ops in C
    mov rdi, rsp                ; rdi = pointer to interrupt_frame_t
    call isr_common_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16                 ; skip vector + error_code
    swapgs                          ; Restore user GS-base before returning
    iretq

; =============================================================================
; Generate all 256 ISR stubs
;
; Error-code vectors have their error code pushed by the CPU:
;   8 (DF), 10 (TS), 11 (NP), 12 (SS), 13 (GP), 14 (PF),
;   17 (AC), 21 (MC), 29 (VC), 30 (SX)
; =============================================================================

%assign i 0
%rep 256
    %if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21 || i == 29 || i == 30
        ISR_ERRCODE i
    %else
        ISR_NOERRCODE i
    %endif
    %assign i i+1
%endrep

; =============================================================================
; Stub address table — one 8-byte pointer per vector
;
; Placed in .rodata so it is read-only and shares a page with other
; const data.  The C side declares this as:
;   extern uint64_t isr_stub_table[256];
; =============================================================================

section .rodata

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr %+ i
    %assign i i+1
%endrep
