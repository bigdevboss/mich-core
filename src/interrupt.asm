global irq1_handler
global irq14_handler
extern irq_handler_main

irq1_handler:
    pusha
    push 1
    call irq_handler_main
    add esp, 4
    popa
    iret

irq14_handler:
    pusha
    push 14
    call irq_handler_main
    add esp, 4
    popa
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
