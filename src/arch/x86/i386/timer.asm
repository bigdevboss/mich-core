global timer_handler
extern schedule_c

timer_handler:
    cld
    pusha

    mov al, 0x20
    out 0x20, al

    ; Mask further IRQs while schedule_c runs. PIT is edge-triggered
    ; so a nested IRQ 0 cannot refire until the next tick, but a disk
    ; or other device can still interrupt and grow the kernel stack
    ; before schedule_c finishes switching tasks.
    cli
    push esp
    call schedule_c
    add esp, 4

    mov esp, eax
    popa
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
