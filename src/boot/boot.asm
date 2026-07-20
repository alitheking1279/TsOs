; =============================================================================
; boot.asm - Multiboot2 32-bit -> 64-bit long mode bootstrap
;
; Boot flow:
;   1. Verify we were actually loaded by a Multiboot2-compliant loader.
;   2. Verify the CPU supports CPUID, long mode, and PAE before using them.
;   3. Build minimal 4-level page tables identity-mapping the first 1 GiB.
;   4. Enable PAE, set the Long Mode Enable bit, enable paging (activates
;      compatibility mode), then load a 64-bit GDT and far-jump into
;      genuine 64-bit long mode.
;   5. Hand control to kernel_main, passing the Multiboot magic and info
;      pointer as its first two arguments (System V AMD64 ABI: rdi, rsi).
;
; Any failed sanity check halts the machine after printing a single
; character error code directly to VGA text memory (0xB8000), since no
; kernel drivers exist yet at this stage. This is the standard bare-metal
; convention (see OSDev Bare Bones / "Rust OS" bootstrap tutorials).
; =============================================================================

bits 32

; -----------------------------------------------------------------------------
; Multiboot2 header (unchanged from original - verified spec-compliant)
; -----------------------------------------------------------------------------
section .multiboot
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
; Boot stack (16-byte aligned, required by SysV ABI at kernel_main entry)
; -----------------------------------------------------------------------------
section .bootstrap_stack, nobits
align 16
stack_bottom:
    resb 16384
stack_top:

; -----------------------------------------------------------------------------
; Constants
; -----------------------------------------------------------------------------
PAGE_PRESENT  equ 1 << 0
PAGE_WRITABLE equ 1 << 1
PAGE_HUGE     equ 1 << 7   ; 2 MiB page (valid at the PD level)

MULTIBOOT2_MAGIC equ 0x36d76289   ; value the loader leaves in EAX

; -----------------------------------------------------------------------------
section .text
global _start
extern kernel_main

_start:
    ; Set up the stack first - everything below uses `call`/`ret`.
    mov esp, stack_top
    mov ebp, esp

    ; EBX holds the physical address of the Multiboot2 info structure.
    ; Save it immediately: CPUID (used below) clobbers EBX, so if we
    ; don't stash this now it is lost forever.
    mov [multiboot_info_ptr], ebx

    ; EAX holds the Multiboot2 magic value. Check it before EAX is
    ; reused by anything else.
    cmp eax, MULTIBOOT2_MAGIC
    jne .err_no_multiboot

    call check_cpuid
    call check_long_mode
    call set_up_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_entry

.err_no_multiboot:
    mov al, '0'
    jmp error

; -----------------------------------------------------------------------------
; check_cpuid
; Confirms CPUID is available by attempting to flip EFLAGS bit 21 (ID).
; If the CPU allows the bit to be flipped, CPUID is supported.
; -----------------------------------------------------------------------------
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax        ; keep original flags in ECX for comparison
    xor eax, 1 << 21     ; try to flip the ID bit
    push eax
    popfd

    pushfd
    pop eax              ; read back what actually stuck

    push ecx
    popfd                ; restore original flags

    cmp eax, ecx
    je .no_cpuid         ; bit didn't change -> no CPUID support
    ret
.no_cpuid:
    mov al, '1'
    jmp error

; -----------------------------------------------------------------------------
; check_long_mode
; Confirms the extended CPUID leaf for long-mode detection exists, then
; confirms long mode itself is supported. Also requires PAE support,
; since we rely on PAE-style page tables below.
; -----------------------------------------------------------------------------
check_long_mode:
    ; Extended functions (>= 0x80000001) must be available first.
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    ; Bit 29 of EDX from leaf 0x80000001 = Long Mode available.
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; Confirm PAE is present too (required for the page table format used).
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
; Builds a minimal PML4 -> PDPT -> PD hierarchy identity-mapping the
; first 1 GiB of physical memory using 2 MiB huge pages. 1 GiB gives the
; early kernel comfortable headroom without needing a 4th page-table
; level; extend later once a real physical memory manager exists.
; -----------------------------------------------------------------------------
set_up_page_tables:
    ; PML4[0] -> PDPT
    mov eax, p3_table
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [p4_table], eax

    ; PDPT[0] -> PD
    mov eax, p2_table
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [p3_table], eax

    ; PD[0..511] -> 512 * 2 MiB huge pages = 1 GiB identity map
    xor ecx, ecx
.map_p2_loop:
    mov eax, ecx
    shl eax, 21                              ; eax = ecx * 2 MiB
    or eax, PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE
    mov [p2_table + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jne .map_p2_loop

    ret

; -----------------------------------------------------------------------------
; enable_paging
; Points CR3 at the PML4, turns on PAE, sets the Long Mode Enable bit in
; EFER, then finally enables paging in CR0 (this is what actually
; activates IA-32e compatibility submode).
; -----------------------------------------------------------------------------
enable_paging:
    mov eax, p4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5              ; CR4.PAE
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
; Prints "ERR: <code>" in white-on-red to the top-left of VGA text mode
; (physical 0xB8000, the standard 80x25 text buffer) then halts forever.
; AL must hold the single-character error code on entry.
; -----------------------------------------------------------------------------
error:
    mov dword [0xb8000], 0x4f524f45   ; "ER" white-on-red
    mov dword [0xb8004], 0x4f3a4f52   ; "R:"
    mov dword [0xb8008], 0x4f204f20   ; "  "
    mov byte  [0xb800a], al
    mov byte  [0xb800b], 0x4f
.halt:
    cli
    hlt
    jmp .halt

; -----------------------------------------------------------------------------
; 64-bit entry point
; -----------------------------------------------------------------------------
bits 64
long_mode_entry:
    ; Long mode uses flat, effectively-unused data segments; null them out.
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Pass the Multiboot2 magic and info-structure pointer to the kernel,
    ; matching the SysV AMD64 ABI (1st arg = rdi, 2nd arg = rsi).
    mov edi, MULTIBOOT2_MAGIC
    mov esi, [multiboot_info_ptr]

    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

; -----------------------------------------------------------------------------
; Data: page tables, GDT, saved boot state
; -----------------------------------------------------------------------------
section .data
align 4096
p4_table:
    times 512 dq 0
p3_table:
    times 512 dq 0
p2_table:
    times 512 dq 0

align 8
multiboot_info_ptr: dd 0   ; physical address of the Multiboot2 info struct

align 8
gdt64:
    dq 0                                          ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)       ; 64-bit code segment
.pointer:
    dw $ - gdt64 - 1
    dq gdt64