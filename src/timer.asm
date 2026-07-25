global timer_handler
extern schedule_c

timer_handler:
    pusha

    mov al, 0x20
    out 0x20, al

    push esp
    call schedule_c
    add esp, 4

    mov esp, eax
    popa
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
