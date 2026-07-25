global syscall_handler
extern syscall_dispatcher

syscall_handler:
    pusha
    push edx
    push ecx
    push ebx
    push eax
    sti
    call syscall_dispatcher
    add esp, 16
    mov [esp + 28], eax
    popa
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
