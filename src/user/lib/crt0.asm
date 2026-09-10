BITS 32

global _start
extern main

%define SYS_EXIT 14

section .text
_start:
    xor ebp, ebp
    and esp, 0xFFFFFFF0
    call main

    mov ebx, eax
    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang
