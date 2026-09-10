; Secondary-CPU (AP) trampoline for SMP bring-up.
;
; QEMU brings each AP up with INIT-SIPI-SIPI. After the SIPI the CPU
; runs in 16-bit real mode with CS = <SIPI page> and EIP = 0 (QEMU's
; cpu_x86_load_seg_cache_sipi sets EIP to the page base, NOT the real
; hardware 0xFFF0), and only CS is rebased - every other segment keeps
; whatever base the firmware left it with. So the entry chunk lives at
; offset 0 of the startup page and code16 loads every segment register
; from TR_SEG before touching any memory.
;
; The BSP copies the chunks below into one 4 KiB page at SMP_TRAMP_BASE
; (0x8000, free: between the boot block and the 1 MiB kernel link base)
; and fills the overlay, so every runtime address derives from that
; base. The kernel PML4 (0x70000) identity maps the low 2 GiB, so the
; page stays reachable once paging is on.
;
; The real -> 32-bit -> 64-bit sequence mirrors the proven boot path in
; bdb2.asm (lgdt, PE, far jump to 32-bit code; PAE/PML4/LME/PG, far jump
; to 64-bit code), and the flat 32-bit code/data descriptors are the
; exact values the boot GDT uses.
;
; The SIPI stub (real mode) is placed separately at 0x580 (SMP64_SIPI_STUB),
; because the 8-bit APIC vector cannot address the 0x8000 page directly.
;
; Runtime layout inside the page (filled by the BSP):
;   0x000  entry  (16-bit real mode, reached via the 0x580 SIPI stub)
;   0x500  overlay: [0]=kernel GDT base(8B), [8]=expected APIC id(4B),
;                   [12]=per-CPU stack top(8B), [20]=AP entry fn(8B)
;   0x100  gdt:  [0]=null, [1]=32-bit code, [2]=32-bit data (flat, G)
;   0x118  gdtr: {u16 limit; u32 base} (6-byte, 16/32-bit lgdt format)
;   0x11E  gdtr_k: {u16 limit; u64 base} (10-byte, 64-bit lgdt format)
;   0x200  code16 (16-bit real mode -> 32-bit protected)
;   0x300  code32 (32-bit protected -> long mode)
;   0x400  code64 (long mode -> AP C entry)

%define TR 0x8000
%define TR_SEG (TR / 16)          ; real-mode segment value whose base is TR

section .rodata.smp_tramp
align 16

; ---- GDT chunk (placed at TR+0x100) -----------------------------------
global smp_tramp_gdt
global smp_tramp_gdt_size
smp_tramp_gdt:
    dq 0
    dq 0x00CF9A000000FFFF        ; [1] 32-bit code, flat (boot-proven)
    dq 0x00CF92000000FFFF        ; [2] 32-bit data, flat (boot-proven)
    dw 23                         ; gdtr.limit (3 entries)
    dd TR + 0x100                 ; gdtr.base
    dw 55                         ; gdtr_k.limit (7 kernel entries)
    dw 0                          ; gdtr_k reserved
    dq 0                          ; gdtr_k base (filled from the overlay)
smp_tramp_gdt_size:
    dd smp_tramp_gdt_end - smp_tramp_gdt
smp_tramp_gdt_end:

; ---- code16 chunk (placed at TR+0x200) --------------------------------
BITS 16
global smp_tramp_code16
global smp_tramp_code16_size
smp_tramp_code16:                 ; 16-bit real mode, all segment bases TR
    cli
    xor ax, ax
    mov ax, TR_SEG                ; only CS is guaranteed based at TR
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    lgdt [0x118]                  ; gdtr (SS base TR -> TR+0x118)
    mov eax, cr0
    or  al, 1
    mov cr0, eax                  ; protected mode
    mov byte [0x551], 'B'          ; progress flag: protected mode (DS base TR)
    jmp 0x0008:0x8300             ; 32-bit code selector @ TR+0x300
smp_tramp_code16_size:
    dd smp_tramp_code16_end - smp_tramp_code16
smp_tramp_code16_end:

; ---- code32 chunk (placed at TR+0x300) --------------------------------
BITS 32
global smp_tramp_code32
global smp_tramp_code32_size
smp_tramp_code32:                 ; 32-bit protected, flat base 0
    cli
    mov ax, 0x0010
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, cr4
    or  eax, (1 << 5) | (1 << 9)  ; PAE + OSFXSR (match the BSP)
    mov cr4, eax
    mov eax, 0x70000              ; kernel PML4 (identity maps this page)
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 1 << 8               ; EFER.LME
    wrmsr
    mov eax, cr0
    or  eax, 1 << 31              ; PG -> long mode (compatibility)
    mov cr0, eax
    ; Install the kernel GDT (base from the overlay @ TR+0x500).
    ; gdtr_k is a 10-byte descriptor: u16 limit at 0x811E, u64 base at 0x8120.
    mov eax, [0x8500]
    mov [0x8120], eax             ; gdtr_k base low
    mov eax, [0x8504]
    mov [0x8124], eax             ; gdtr_k base high
    lgdt [0x811E]
    jmp 0x0008:0x8400             ; kernel 64-bit code @ TR+0x400
smp_tramp_code32_size:
    dd smp_tramp_code32_end - smp_tramp_code32
smp_tramp_code32_end:

; ---- code64 chunk (placed at TR+0x400) --------------------------------
BITS 64
global smp_tramp_code64
global smp_tramp_code64_size
smp_tramp_code64:                 ; long mode, kernel GDT
    mov ax, 0x0010
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov byte [0x8552], 'C'          ; progress flag: long mode reached (flat DS)
    mov rsp, [0x8510]             ; per-CPU stack top (overlay)
    mov rdi, [0x8508]             ; expected APIC id (overlay)
    call qword [0x8518]           ; AP entry function (overlay)
.halt64:
    hlt
    jmp .halt64
smp_tramp_code64_size:
    dd smp_tramp_code64_end - smp_tramp_code64
smp_tramp_code64_end:

; ---- entry chunk (placed at 0x000, the SIPI target) -------------------
BITS 16
global smp_tramp_entry
global smp_tramp_entry_size
smp_tramp_entry:                  ; 16-bit real mode, only CS based at TR
    cli
    mov byte [cs:0x550], 'A'      ; progress flag (NO serial: the BSP shares
    jmp TR_SEG:0x0200             ; the UART with us - see smp64.h AP flags)
smp_tramp_entry_size:
    dd smp_tramp_entry_end - smp_tramp_entry
smp_tramp_entry_end:

section .note.GNU-stack noalloc noexec nowrite progbits
