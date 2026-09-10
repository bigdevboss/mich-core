BITS 64

global _start
extern kernel64_main

section .bss
align 16
stack_bottom:
    ; kernel64_main plus test_tcp: many struct tcp_transmit (~1.5KiB each)
    ; stay live for the whole function, so 32KiB overflowed into .rodata.
    resb 131072
stack_top:

section .text
_start:
    mov rsp, stack_top
    xor rbp, rbp
    call kernel64_main
.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
