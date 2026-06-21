; boot.asm — Multiboot header, 32-bit entry, and the jump into 64-bit long mode.
;
; QEMU's built-in multiboot loader reads the header below and starts us in
; 32-bit protected mode. We verify the CPU supports long mode, build a minimal
; set of page tables (identity-mapping the first 1 GiB), enable paging + long
; mode, load a 64-bit GDT, and finally far-jump into 64-bit code.

global start
extern kernel_main

; ---------------------------------------------------------------------------
; Multiboot v1 header — must be in the first 8 KiB of the file, 4-byte aligned.
; ---------------------------------------------------------------------------
MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0x0
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

; ---------------------------------------------------------------------------
; 32-bit entry point
; ---------------------------------------------------------------------------
section .text
bits 32
start:
    mov esp, stack_top          ; set up a stack

    call check_multiboot
    call check_cpuid
    call check_long_mode

    call set_up_page_tables
    call enable_paging

    lgdt [gdt64.pointer]        ; load the 64-bit GDT
    jmp gdt64.code:long_mode_start  ; far jump into 64-bit code

    hlt

; --- Sanity checks --------------------------------------------------------

check_multiboot:
    cmp eax, 0x2BADB002         ; multiboot v1 loader leaves this magic in eax
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "0"
    jmp error

check_cpuid:
    ; CPUID is supported if we can flip bit 21 (ID) in EFLAGS.
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
    mov al, "1"
    jmp error

check_long_mode:
    mov eax, 0x80000000         ; is the extended CPUID leaf available?
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29           ; LM bit
    jz .no_long_mode
    ret
.no_long_mode:
    mov al, "2"
    jmp error

; --- Paging ---------------------------------------------------------------

set_up_page_tables:
    ; Link P4[0] -> P3, P3[0] -> P2.
    mov eax, p3_table
    or eax, 0b11                ; present + writable
    mov [p4_table], eax

    mov eax, p2_table
    or eax, 0b11
    mov [p3_table], eax

    ; Identity-map 1 GiB using 512 * 2 MiB huge pages.
    mov ecx, 0
.map_p2_table:
    mov eax, 0x200000           ; 2 MiB
    mul ecx                     ; eax = 2 MiB * ecx
    or eax, 0b10000011          ; present + writable + huge
    mov [p2_table + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jne .map_p2_table
    ret

enable_paging:
    mov eax, p4_table           ; load P4 into CR3
    mov cr3, eax

    mov eax, cr4                ; enable PAE
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080         ; EFER MSR
    rdmsr
    or eax, 1 << 8              ; set long-mode bit
    wrmsr

    mov eax, cr0                ; enable paging
    or eax, 1 << 31
    mov cr0, eax
    ret

; Print "ERR: X" (X = error code in al) to the top-left of the screen, then halt.
error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte  [0xb800a], al
    hlt

; ---------------------------------------------------------------------------
; 64-bit entry point
; ---------------------------------------------------------------------------
bits 64
long_mode_start:
    ; Reload all data segment registers with the null selector.
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call kernel_main            ; hand off to C

.hang:
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
; Reserved memory: page tables + stack
; ---------------------------------------------------------------------------
section .bss
align 4096
p4_table:
    resb 4096
p3_table:
    resb 4096
p2_table:
    resb 4096
stack_bottom:
    resb 4096 * 4               ; 16 KiB stack
stack_top:

; ---------------------------------------------------------------------------
; 64-bit GDT: one null descriptor + one 64-bit code descriptor.
; ---------------------------------------------------------------------------
section .rodata
gdt64:
    dq 0                                        ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)    ; exec + descriptor type + present + 64-bit
.pointer:
    dw $ - gdt64 - 1
    dq gdt64
