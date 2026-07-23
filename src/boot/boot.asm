; =============================================================================
; boot.asm - Multiboot2 32-bit -> 64-bit long mode bootstrap (higher-half)
;
; Boot flow:
;   1. Verify we were actually loaded by a Multiboot2-compliant loader.
;   2. Verify the CPU supports CPUID, long mode, and PAE before using them.
;   3. Build minimal 4-level page tables:
;        - Identity map first 1 GiB via PML4[0]   (for 32-bit code)
;        - Higher-half map    via PML4[511]        (for kernel at 0xFFFFFFFF80000000+)
;   4. Enable PAE, set the Long Mode Enable bit, enable paging (activates
;      compatibility mode), then load a 64-bit GDT and far-jump into
;      genuine 64-bit long mode.
;   5. In 64-bit mode, switch to the higher-half kernel stack and call
;      kernel_main with the Multiboot magic and info pointer.
;
; Section layout:
;   .boot.text / .boot.data  — linked at physical 1M (for32-bit addressing)
;   .text (higher-half)      — long_mode_entry lives here (but is reached
;                              via far jump from physical code)
;   .bootstrap_stack         — kernel stack at higher-half VMA
;
; Any failed sanity check halts the machine after printing a single
; character error code directly to VGA text memory (0xB8000), since no
; kernel drivers exist yet at this stage.
; =============================================================================

bits 32

; =============================================================================
; .boot.text — 32-bit code that runs at physical addresses
; =============================================================================
section .boot.text

; -----------------------------------------------------------------------------
; Multiboot2 header (must be in first 32 KiB of loaded image)
; -----------------------------------------------------------------------------
align 8
multiboot_start:
    dd 0xe85250d6                                              ; magic
    dd 0                                                       ; architecture: i386
    dd multiboot_end - multiboot_start                         ; header length
    dd 0x100000000 - (0xe85250d6 + 0 + (multiboot_end - multiboot_start)) ; checksum
    ; end tag (type 0, flags 0, size 8)
    dw 0
    dw 0
    dd 8
multiboot_end:

; -----------------------------------------------------------------------------
; Constants
; -----------------------------------------------------------------------------
PAGE_PRESENT  equ 1 << 0
PAGE_WRITABLE equ 1 << 1
PAGE_HUGE     equ 1 << 7   ; 2 MiB page (valid at the PD level)

MULTIBOOT2_MAGIC equ 0x36d76289   ; value the loader leaves in EAX

; -----------------------------------------------------------------------------
; Entry point — linked at physical 1M
; -----------------------------------------------------------------------------
global _start
extern kernel_main

_start:
    ; Use the boot stack in .boot.data (physical address, always accessible).
    mov esp, boot_stack_top
    mov ebp, esp

    ; EBX holds the physical address of the Multiboot2 info structure.
    ; Save it immediately: CPUID (used below) clobbers EBX.
    mov [mb2_info_save], ebx

    ; EAX holds the Multiboot2 magic value.
    cmp eax, MULTIBOOT2_MAGIC
    jne .err_no_multiboot

    call check_cpuid
    call check_long_mode
    call set_up_page_tables
    call enable_paging

    ; Paging is now on — identity map still active so physical addresses work.
    ; Load the 64-bit GDT (at physical address in .boot.data).
    lgdt [gdt64.pointer]

    ; Far jump into 64-bit long mode — target is in this same section
    ; (physical address, identity-mapped).
    jmp gdt64.code:long_mode_entry

.err_no_multiboot:
    mov al, '0'
    jmp error

; -----------------------------------------------------------------------------
; check_cpuid
; Confirms CPUID is available by attempting to flip EFLAGS bit 21 (ID).
; -----------------------------------------------------------------------------
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd

    pushfd
    pop eax

    push ecx
    popfd

    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, '1'
    jmp error

; -----------------------------------------------------------------------------
; check_long_mode
; Confirms long mode and PAE are supported.
; -----------------------------------------------------------------------------
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    mov eax, 1
    cpuid
    test edx, 1 << 6
    jz .no_long_mode

    ret
.no_long_mode:
    mov al, '2'
    jmp error

; -----------------------------------------------------------------------------
; set_up_page_tables
;
; Builds two mappings:
;   1. Identity: PML4[0] -> p3_table -> p2_table -> 512*2MiB = first 1 GiB
;   2. Higher-half: PML4[511] -> p3_table_hh[510] -> p2_table_hh -> same 1 GiB
;      mapped at virtual 0xFFFFFFFF80000000..0xFFFFFFFFC0000000
;
; VA decomposition for 0xFFFFFFFF80000000:
;   PML4 index  = 511  (bits 47:39)
;   PDPT index  = 510  (bits 38:30)
;   PD   index  =   0  (bits 29:21)
;   PT   index  =   0  (bits 20:12)
; -----------------------------------------------------------------------------
set_up_page_tables:
    ; === PML4 entries ===

    ; PML4[0] -> identity PDPT
    mov eax, p3_table
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [p4_table], eax

    ; PML4[511] -> higher-half PDPT
    mov eax, p3_table_hh
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [p4_table + 511*8], eax

    ; === Identity PDPT ===

    ; PDPT[0] -> identity PD (first 1 GiB)
    mov eax, p2_table
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [p3_table], eax

    ; === Higher-half PDPT ===

    ; PDPT[510] -> higher-half PD (maps 0xFFFFFFFF80000000..C0000000)
    mov eax, p2_table_hh
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [p3_table_hh + 510*8], eax

    ; === Identity PD: 512 * 2 MiB huge pages for physical 0..1 GiB ===
    xor ecx, ecx
.map_p2_loop:
    mov eax, ecx
    shl eax, 21                              ; eax = ecx * 2 MiB
    or eax, PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE
    mov [p2_table + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jne .map_p2_loop

    ; === Higher-half PD: same physical 0..1 GiB at VA 0xFFFFFFFF80000000+ ===
    xor ecx, ecx
.map_p2_hh_loop:
    mov eax, ecx
    shl eax, 21
    or eax, PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE
    mov [p2_table_hh + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jne .map_p2_hh_loop

    ret

; -----------------------------------------------------------------------------
; enable_paging
; Points CR3 at the PML4, turns on PAE, sets EFER.LME + EFER.NXE,
; then enables CR0.PG and CR0.WP.
; -----------------------------------------------------------------------------
enable_paging:
    mov eax, p4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5              ; CR4.PAE
    or eax, 1 << 9              ; CR4.OSFXSR — enable FXSAVE/FXRSTOR
    or eax, 1 << 7              ; CR4.PGE — enable global pages
    mov cr4, eax

    mov ecx, 0xC0000080         ; IA32_EFER MSR
    rdmsr
    or eax, 1 << 8 | 1 << 11   ; EFER.LME | EFER.NXE
    wrmsr

    mov eax, cr0
    or eax, 1 << 31 | 1 << 16  ; CR0.PG | CR0.WP
    mov cr0, eax

    ret

; -----------------------------------------------------------------------------
; error
; Prints "ERR: <code>" in white-on-red to VGA text memory then halts.
; -----------------------------------------------------------------------------
error:
    mov dword [0xb8000], 0x4f524f45   ; "ER"
    mov dword [0xb8004], 0x4f3a4f52   ; "R:"
    mov dword [0xb8008], 0x4f204f20   ; "  "
    mov byte  [0xb800a], al
    mov byte  [0xb800b], 0x4f
.halt:
    cli
    hlt
    jmp .halt

; =============================================================================
; .boot.data — page tables, GDT, saved state (all at physical addresses)
; =============================================================================
section .boot.data

align 4096
p4_table:
    times 512 dq 0
p3_table:
    times 512 dq 0
p2_table:
    times 512 dq 0
p3_table_hh:
    times 512 dq 0
p2_table_hh:
    times 512 dq 0

align 8
mb2_info_save: dd 0

align 8
gdt64:
    dq 0                                          ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)       ; 64-bit code segment
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

; Boot stack — used only during 32-bit init, small to save space.
align 16
boot_stack_bottom:
    resb 4096
boot_stack_top:

; =============================================================================
; 64-bit entry point — still in .boot.text (physical address, identity-mapped)
;
; After the far jump from 32-bit mode we are in genuine 64-bit long mode.
; Both identity and higher-half page table entries are active, so we can
; access any physical address AND the higher-half kernel addresses.
; =============================================================================
section .boot.text
bits 64

long_mode_entry:
    ; Clear segment registers (flat model).
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Switch to the higher-half kernel stack.
    mov rsp, stack_top
    mov rbp, rsp

    ; Pass Multiboot2 magic and info pointer to kernel_main.
    ; rdi = magic (1st arg, SysV ABI), rsi = info_ptr (2nd arg).
    mov edi, MULTIBOOT2_MAGIC
    mov esi, [mb2_info_save]

    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

; =============================================================================
; Higher-half kernel stack — in the kernel's .bootstrap_stack section,
; linked at the higher-half VMA (0xFFFFFFFF80...).  Accessible once the
; higher-half page tables are active.
; =============================================================================
section .bootstrap_stack, nobits
align 16
stack_bottom:
    resb 16384
stack_top:
