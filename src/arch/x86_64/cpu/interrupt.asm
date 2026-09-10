BITS 64

global timer64_entry
global spurious64_entry
extern timer64_dispatch
extern platform64_eoi
extern irq64_dispatch

section .text
timer64_entry:
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
    call timer64_dispatch
    call platform64_eoi
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
    iretq

spurious64_entry:
    iretq


%macro IRQ64 1
irq64_%1:
    push qword 0
    push qword %1
    jmp irq64_common
%endmacro

%assign vector 48
%rep 16
IRQ64 vector
%assign vector vector+1
%endrep

%assign vector 65
%rep 15
IRQ64 vector
%assign vector vector+1
%endrep

%assign vector 80
%rep 144
IRQ64 vector
%assign vector vector+1
%endrep

irq64_common:
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
    mov rdi, [rsp + 120]
    call irq64_dispatch
    call platform64_eoi
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
global irq64_table
irq64_table:
%assign vector 48
%rep 16
    dq irq64_%+vector
%assign vector vector+1
%endrep

align 8
global msi64_table
msi64_table:
%assign vector 80
%rep 144
    dq irq64_%+vector
%assign vector vector+1
%endrep

align 8
global smp64_ipi_table
smp64_ipi_table:
%assign vector 65
%rep 15
    dq irq64_%+vector
%assign vector vector+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
