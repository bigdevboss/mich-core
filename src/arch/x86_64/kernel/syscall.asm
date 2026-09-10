BITS 64

global syscall64_entry
extern syscall64_dispatch
extern syscall64_validate_return

; Offsets into struct smp64_syscall at GS base after SWAPGS.
; Must match smp64.h.
%define SC_KERNEL_RSP 0
%define SC_RSP        8
%define SC_RDI        16
%define SC_RSI        24
%define SC_RDX        32
%define SC_R8         40
%define SC_R9         48
%define SC_R10        56
%define SC_RIP        64
%define SC_RFLAGS     72
%define SC_RBX        80
%define SC_RBP        88
%define SC_R12        96
%define SC_R13        104
%define SC_R14        112
%define SC_R15        120

section .text
syscall64_entry:
    swapgs
    mov [gs:SC_RDI], rdi
    mov [gs:SC_RSI], rsi
    mov [gs:SC_RDX], rdx
    mov [gs:SC_R8], r8
    mov [gs:SC_R9], r9
    mov [gs:SC_R10], r10
    mov [gs:SC_RSP], rsp
    mov [gs:SC_RIP], rcx
    mov [gs:SC_RFLAGS], r11
    mov [gs:SC_RBX], rbx
    mov [gs:SC_RBP], rbp
    mov [gs:SC_R12], r12
    mov [gs:SC_R13], r13
    mov [gs:SC_R14], r14
    mov [gs:SC_R15], r15
    mov rsp, [gs:SC_KERNEL_RSP]
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax
    call syscall64_dispatch
    mov rdi, rax
    call syscall64_validate_return
    mov rdi, [gs:SC_RDI]
    mov rsi, [gs:SC_RSI]
    mov rdx, [gs:SC_RDX]
    mov r8, [gs:SC_R8]
    mov r9, [gs:SC_R9]
    mov r10, [gs:SC_R10]
    mov rbx, [gs:SC_RBX]
    mov rbp, [gs:SC_RBP]
    mov r12, [gs:SC_R12]
    mov r13, [gs:SC_R13]
    mov r14, [gs:SC_R14]
    mov r15, [gs:SC_R15]
    mov rcx, [gs:SC_RIP]
    mov r11, [gs:SC_RFLAGS]
    mov rsp, [gs:SC_RSP]
    swapgs
    o64 sysret

section .note.GNU-stack noalloc noexec nowrite progbits
