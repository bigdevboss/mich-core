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
%define SC_SIG_RAX    128
%define SC_SIG_RCX    136
%define SC_SIG_R11    144
%define SC_CTXRES     152

; The sentinel syscall64_validate_return answers a sigreturn with. Must
; match POSIX_SIGRETURN_SENTINEL in posix_abi.h.
%define SIGRETURN_SENTINEL 0x5349475245544952

section .text
syscall64_entry:
    swapgs
    mov qword [gs:SC_CTXRES], 0
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
    ; A 64-bit immediate has no cmp form, so the sentinel compare runs
    ; through the stack: rdx and friends are already restored user
    ; registers and cannot serve as scratch.
    push rax
    mov rax, SIGRETURN_SENTINEL
    cmp rax, [rsp]
    pop rax
    je .resume_full
    ; A park, exit, or exec under this syscall switched the task context:
    ; the resumed task never ran a syscall instruction, so folding rcx
    ; into its rip would silently clobber its registers. Resume through
    ; the same iret path the sigreturn takes.
    cmp qword [gs:SC_CTXRES], 0
    jne .resume_full
    jmp .plain_sysret
    ; A sigreturn resumes the interrupted register set in full, which
    ; sysret cannot do: it forces rcx to the rip and r11 to the flags.
    ; The iret frame below reloads rip, rsp, and flags from the per-CPU
    ; slot and keeps the saved rcx, r11, and rax in their registers.
.resume_full:
    mov rax, [gs:SC_SIG_RAX]
    mov rcx, [gs:SC_SIG_RCX]
    mov r11, [gs:SC_SIG_R11]
    push qword 0x1B
    push qword [gs:SC_RSP]
    push qword [gs:SC_RFLAGS]
    push qword 0x23
    push qword [gs:SC_RIP]
    swapgs
    iretq
.plain_sysret:
    mov rcx, [gs:SC_RIP]
    mov r11, [gs:SC_RFLAGS]
    mov rsp, [gs:SC_RSP]
    swapgs
    o64 sysret

section .note.GNU-stack noalloc noexec nowrite progbits
