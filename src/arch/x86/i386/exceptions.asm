extern exception_dispatch
global exc_unknown
global exc_table
global exc9

%macro EXCN 1
exc%1:
    push dword 0
    push dword %1
    jmp exc_common
%endmacro

%macro EXCE 1
exc%1:
    push dword %1
    jmp exc_common
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

exc_unknown:
    push dword 0
    push dword 0xFF
    jmp exc_common

exc_common:
    cld
    pusha
    mov eax, [esp + 32]
    mov ebx, [esp + 36]
    mov ecx, cr2
    mov edx, [esp + 40]
    mov esi, [esp + 44]
    push esi
    push edx
    push ecx
    push ebx
    push eax
    call exception_dispatch
    add esp, 20
    popa
    add esp, 8
    iret

section .data
align 4
exc_table:
    dd exc0
    dd exc1
    dd exc2
    dd exc3
    dd exc4
    dd exc5
    dd exc6
    dd exc7
    dd exc8
    dd exc9
    dd exc10
    dd exc11
    dd exc12
    dd exc13
    dd exc14
    dd exc15
    dd exc16
    dd exc17
    dd exc18
    dd exc19
    dd exc20
    dd exc21
    dd exc22
    dd exc23
    dd exc24
    dd exc25
    dd exc26
    dd exc27
    dd exc28
    dd exc29
    dd exc30
    dd exc31

section .note.GNU-stack noalloc noexec nowrite progbits
