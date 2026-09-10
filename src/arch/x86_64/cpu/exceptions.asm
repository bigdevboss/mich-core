BITS 64

extern exception64_dispatch
global exception64_table

%macro EXCN 1
global exception64_%1
exception64_%1:
    push qword 0
    push qword %1
    jmp exception64_common
%endmacro

%macro EXCE 1
global exception64_%1
exception64_%1:
    push qword %1
    jmp exception64_common
%endmacro

section .text
EXCN 0
EXCN 1
EXCN 2
EXCN 3
EXCN 4
EXCN 5
EXCN 6
EXCN 7
EXCE 8
EXCN 9
EXCE 10
EXCE 11
EXCE 12
EXCE 13
EXCE 14
EXCN 15
EXCN 16
EXCE 17
EXCN 18
EXCN 19
EXCN 20
EXCE 21
EXCN 22
EXCN 23
EXCN 24
EXCN 25
EXCN 26
EXCN 27
EXCN 28
EXCE 29
EXCE 30
EXCN 31

exception64_common:
    cld
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    call exception64_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

section .rodata
align 8
exception64_table:
%assign i 0
%rep 32
    dq exception64_%+i
%assign i i+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
