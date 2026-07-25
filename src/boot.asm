section .multiboot2_header
header_start:
    dd 0xE85250D6
    dd 0
    dd header_end - header_start
    dd 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start))

    align 8
fb_tag_start:
    dw 5
    dw 0
    dd fb_tag_end - fb_tag_start
    dd 0
    dd 0
    dd 32
fb_tag_end:

    align 8
    dw 0
    dw 0
    dd 8
header_end:

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    push ebx
    push eax
    call kernel_main
    cli
    hlt
