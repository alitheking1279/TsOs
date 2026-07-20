; =============================================================================
; gdt_flush.asm — Install a new GDT and reload all segment registers.
;
; Calling convention: System V AMD64 ABI throughout.
; Callee-saved registers (rbx, rbp, r12-r15) are not touched.
; =============================================================================

bits 64
section .text

; -----------------------------------------------------------------------------
; gdt_flush — atomically switch to a new GDT and reload every segment register
;
; Prototype (C):
;   void gdt_flush(gdt_pointer_t *ptr, uint64_t cs, uint64_t ds);
;
; Arguments (SysV AMD64 ABI):
;   rdi — pointer to a packed {uint16_t limit, uint64_t base} pseudo-descriptor
;   rsi — new CS selector value  (e.g. GDT_KERNEL_CS_SEL = 0x08)
;   rdx — new DS/ES/FS/GS/SS selector value (e.g. GDT_KERNEL_DS_SEL = 0x10)
;
; Why assembly?
;   Reloading CS in 64-bit mode cannot be expressed in standard C.  The only
;   portable way is a 64-bit far transfer.  We use a far return (retfq) rather
;   than a far jump because a far jump would require a memory operand with a
;   linker-time address, while retfq lets us build the target address with a
;   RIP-relative LEA (position-independent, no link-time fixup needed).
;
; Stack layout built before retfq:
;   [rsp+0]  new RIP (.reload)        ← retfq pops this into RIP
;   [rsp+8]  new CS  (value of rsi)   ← retfq pops this into CS
; -----------------------------------------------------------------------------
global gdt_flush
gdt_flush:
    lgdt [rdi]                      ; load new GDTR from *ptr

    ; Build the far-return frame manually.
    ; retfq pops [rsp] -> RIP then [rsp+8] -> CS, so push CS first.
    push rsi                        ; [future rsp+8] = new CS selector
    lea  rax, [rel .reload]         ; RIP-relative address of .reload label
    push rax                        ; [rsp+0] = new RIP
    retfq                           ; CS <- rsi, RIP <- .reload

.reload:
    ; All data-class segment registers now get the new DS selector.
    ; In 64-bit mode these are not used for addressing, but the CPU still
    ; enforces DPL checks when they are loaded and when a privilege
    ; transition occurs.  Loading them with the correct kernel selector
    ; avoids #GP faults if any future code inspects them.
    ;
    ; Note: mov ss, ax temporarily inhibits interrupts for the following
    ; instruction (same behaviour as in 32-bit mode); this is safe here
    ; because interrupts have not been enabled yet at boot time.
    mov ax, dx                      ; dx holds ds selector (lower 16 bits of rdx)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; -----------------------------------------------------------------------------
; tss_flush — load the Task Register with the given TSS selector
;
; Prototype (C):
;   void tss_flush(uint64_t tss_sel);
;
; Arguments:
;   rdi — TSS segment selector (e.g. GDT_TSS_SEL = 0x28)
;
; ltr marks the TSS descriptor in the GDT as Busy (type 0x9 -> 0xB).
; Must be called after gdt_flush() so the GDTR already points to a GDT that
; contains a valid TSS system descriptor at the given selector.
; -----------------------------------------------------------------------------
global tss_flush
tss_flush:
    ltr di                          ; di = lower 16 bits of rdi = TSS selector
    ret
