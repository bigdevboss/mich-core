BITS 64

global gdt64_load
global gdt64_ltr
global user64_enter

gdt64_load:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    push qword 0x08
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    ret

gdt64_ltr:
    mov ax, di
    ltr ax
    ret

user64_enter:
    mov rax, rdi
    mov r12, rdx
    mov rdi, rdx
    mov dx, 0x1B
    mov ds, dx
    mov es, dx
    push qword 0x1B
    push rsi
    push qword 0x202
    push qword 0x23
    push rax
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
