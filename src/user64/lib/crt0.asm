BITS 64

global _start
extern main

%define SYS_EXIT 14

section .text
_start:
    xor rbp, rbp
    and rsp, -16
    call main
    mov edi, eax
    mov eax, SYS_EXIT
    syscall
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
