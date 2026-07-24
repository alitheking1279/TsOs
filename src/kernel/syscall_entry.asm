; =============================================================================
; syscall_entry.asm — SYSCALL/SYSRET trampoline
;
; Entry point programmed into MSR_LSTAR.  On SYSCALL:
;   - CPU loads RIP from LSTAR (this function)
;   - CPU loads CS = kernel CS, SS = kernel SS (from STAR)
;   - CPU saves user RIP into RCX, user RFLAGS into R11
;   - CPU loads RSP from TSS.RSP0 (ring-0 stack)
;   - CPU clears RFLAGS.IF (masked by SFMASK)
;
; This trampoline:
;   1. Loads the kernel stack pointer from per-CPU data (GS:[0])
;   2. Saves all general-purpose registers onto the kernel stack
;   3. Calls the C syscall_handler(frame_ptr)
;   4. Restores registers and returns to user mode via SYSRET
;
; Frame layout (offsets from frame pointer passed to C handler):
;   +0   rax   (syscall number / return value)
;   +8   r10   (arg4)
;   +16  r9    (arg5)
;   +24  r8    (arg6)
;   +32  rdx   (arg3)
;   +40  rsi   (arg2)
;   +48  rdi   (arg1)
;   +56  rbx
;   +64  rbp
;   +72  r12
;   +80  r13
;   +88  r14
;   +96  r15
;   +104 r11  (user RFLAGS, saved separately)
;   +112 rcx  (user RIP, saved separately)
;
; References:
;   Intel SDM Vol.3A §3.3.1  — SYSCALL/SYSRET
;   AMD APM Vol.2 §3.3       — SYSCALL/SYSRET in long mode
; =============================================================================

bits 64
section .text

extern syscall_handler

global syscall_entry
syscall_entry:
    ; --- Swap GS: kernel GS now accessible via GS:[0] ---
    swapgs

    ; --- Load kernel RSP from per-CPU data ---
    mov rsp, qword [gs:0]

    ; --- Build the syscall frame (push order = reverse of offset order) ---
    push rax        ; [+0]   syscall number (C handler will put return val here)
    push r10        ; [+8]   arg4
    push r9         ; [+16]  arg5
    push r8         ; [+24]  arg6
    push rdx        ; [+32]  arg3
    push rsi        ; [+40]  arg2
    push rdi        ; [+48]  arg1
    push rbx        ; [+56]
    push rbp        ; [+64]
    push r12        ; [+72]
    push r13        ; [+80]
    push r14        ; [+88]
    push r15        ; [+96]
    push r11        ; [+104] user RFLAGS
    push rcx        ; [+112] user RIP (return address for SYSRET)

    ; --- rdi = frame pointer (first argument for syscall_handler) ---
    mov rdi, rsp

    ; --- Call the C dispatcher ---
    call syscall_handler

    ; --- Restore r11 (RFLAGS) and rcx (RIP) from the frame ---
    pop rcx         ; [+112] user RIP
    pop r11         ; [+104] user RFLAGS

    ; --- Restore general-purpose registers from the frame ---
    pop r15         ; [+96]
    pop r14         ; [+88]
    pop r13         ; [+80]
    pop r12         ; [+72]
    pop rbp         ; [+64]
    pop rbx         ; [+56]
    pop rdi         ; [+48]  (overwritten, but keep stack balanced)
    pop rsi         ; [+40]  (overwritten)
    pop rdx         ; [+32]  (overwritten)
    pop r8          ; [+24]  (overwritten)
    pop r9          ; [+16]  (overwritten)
    pop r10         ; [+8]   (overwritten)
    pop rax         ; [+0]   return value

    ; --- Return to user mode ---
    swapgs
    sysret
