; POSIX CRT entry: the kernel-built initial stack (profile 4.7) carries
; argc, argv, envp; no semantic argument arrives in RDI.
BITS 64

global _start
global environ
extern main

%define SYS_POSIX_EXIT 204

section .text
_start:
    xor rbp, rbp
    and rsp, -16
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    lea rax, [rdi + 1]
    shl rax, 3
    lea rdx, [rsi + rax]
    mov rax, environ
    mov [rax], rdx
    call main
    mov edi, eax
    mov eax, SYS_POSIX_EXIT
    syscall
.hang:
    jmp .hang

section .data
environ: dq 0

section .note.GNU-stack noalloc noexec nowrite progbits
