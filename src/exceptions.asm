global exception_handler
extern panic

exception_handler:
    pop eax
    mov ecx, cr2
    push ecx
    push eax
    call panic
    add esp, 8
    iret
