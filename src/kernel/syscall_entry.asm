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
;   1. Saves the user RSP into per-CPU scratch (GS:[8]) and loads the kernel
;      stack pointer from per-CPU data (GS:[0])
;   2. Saves all general-purpose registers onto the kernel stack
;   3. Calls the C syscall_handler(frame_ptr)
;   4. Restores registers (incl. user RSP) and returns to user mode via SYSRET
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

    ; --- Save user RSP (SYSRET does not restore it), then load kernel RSP ---
    mov [gs:8], rsp
    mov rsp, qword [gs:0]

    ; --- Build the syscall frame (reverse of the frame-layout offsets) ---
    ; The C handler indexes frame[i] = *(frame_ptr + 8*i) with the offsets
    ; listed above (SC_OFF_RAX=0, SC_OFF_R10=8, ... SC_OFF_RCX=112).  That
    ; means the rax slot must sit at the LOWEST address, i.e. rax is pushed
    ; LAST.  Pushing rax first would place it at the highest address, and
    ; frame[6] (RDI) would read 40 bytes above the save area (stale stack).
    push rcx        ; [+112] user RIP (return address for SYSRET)
    push r11        ; [+104] user RFLAGS
    push r15        ; [+96]
    push r14        ; [+88]
    push r13        ; [+80]
    push r12        ; [+72]
    push rbp        ; [+64]
    push rbx        ; [+56]
    push rdi        ; [+48]   arg1
    push rsi        ; [+40]   arg2
    push rdx        ; [+32]   arg3
    push r8         ; [+24]   arg6
    push r9         ; [+16]   arg5
    push r10        ; [+8]    arg4
    push rax        ; [+0]    syscall number (C handler puts return value here)

    ; --- Point rdi at the frame base (rax slot, now the lowest address) ---
    mov rdi, rsp
    call syscall_handler

    ; --- Restore general-purpose registers (reverse of the push order) ---
    pop rax         ; [+0]   return value
    pop r10         ; [+8]   (overwritten)
    pop r9          ; [+16]  (overwritten)
    pop r8          ; [+24]  (overwritten)
    pop rdx         ; [+32]  (overwritten)
    pop rsi         ; [+40]  (overwritten)
    pop rdi         ; [+48]  (overwritten, but keep stack balanced)
    pop rbx         ; [+56]
    pop rbp         ; [+64]
    pop r12         ; [+72]
    pop r13         ; [+80]
    pop r14         ; [+88]
    pop r15         ; [+96]
    pop r11         ; [+104] user RFLAGS
    pop rcx         ; [+112] user RIP

    ; --- Restore user RSP, then return to user mode ---
    ; MUST be the 64-bit operand-size SYSRET.  A bare `sysret` (0F 07)
    ; returns to 32-bit compatibility mode: CS = STAR[63:48] = 0x13, EIP/ESP
    ; truncated to 32 bits.  `o64 sysret` (REX.W, 48 0F 07) sets CS =
    ; STAR[63:48]+16 = 0x23 and preserves full 64-bit RIP/RSP.  NOTE: NASM <
    ; ~2.x does not know the AT&T mnemonic `sysretq` and would treat the line
    ; as a label — use the explicit operand-size prefix instead.
    mov rsp, qword [gs:8]
    swapgs
    o64 sysret
