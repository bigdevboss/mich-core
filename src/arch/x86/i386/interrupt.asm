extern irq_handler_main

global irq_table

%macro IRQ_STUB 1
global irq%1_handler
irq%1_handler:
    cld
    pusha
    push dword %1
    call irq_handler_main
    add esp, 4
    popa
    iret
%endmacro

section .text

IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

section .rodata
align 4
irq_table:
    dd irq1_handler
    dd irq2_handler
    dd irq3_handler
    dd irq4_handler
    dd irq5_handler
    dd irq6_handler
    dd irq7_handler
    dd irq8_handler
    dd irq9_handler
    dd irq10_handler
    dd irq11_handler
    dd irq12_handler
    dd irq13_handler
    dd irq14_handler
    dd irq15_handler

section .note.GNU-stack noalloc noexec nowrite progbits
