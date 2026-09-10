BITS 32

global mich_write
global mich_clear
global mich_send
global mich_recv
global mich_send_nb
global mich_send_timeout
global mich_recv_from
global mich_memfree
global mich_fork
global mich_exec
global mich_exit
global mich_wait
global mich_getpid
global mich_kill
global mich_service_register
global mich_service_lookup
global mich_cap_get
global mich_cap_drop
global mich_cap_grant
global mich_irq_register
global mich_irq_grant
global mich_ioport_grant
global mich_mmio_grant
global mich_mmio_map
global mich_dma_grant
global mich_dma_alloc
global mich_yield

%define SYS_WRITE   1
%define SYS_CLEAR   4
%define SYS_SEND    5
%define SYS_RECV    6
%define SYS_SEND_NB 7
%define SYS_IRQ_REG 8
%define SYS_MEMFREE 11
%define SYS_FORK    12
%define SYS_EXEC    13
%define SYS_EXIT    14
%define SYS_WAIT    15
%define SYS_GETPID  16
%define SYS_KILL    17
%define SYS_SERVICE_REGISTER 18
%define SYS_SERVICE_LOOKUP   19
%define SYS_CAP_GET          20
%define SYS_CAP_DROP         21
%define SYS_CAP_GRANT        22
%define SYS_YIELD            23
%define SYS_RECV_FROM        24
%define SYS_IRQ_GRANT        25
%define SYS_IOPORT_GRANT     26
%define SYS_MMIO_GRANT       27
%define SYS_MMIO_MAP         28
%define SYS_DMA_GRANT        29
%define SYS_DMA_ALLOC        30
%define SYS_SEND_TIMEOUT     31

section .text
mich_write:
    push ebx
    mov ebx, [esp + 8]
    mov eax, SYS_WRITE
    int 0x80
    pop ebx
    ret

mich_clear:
    mov eax, SYS_CLEAR
    int 0x80
    ret

mich_send:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov eax, SYS_SEND
    int 0x80
    pop ebx
    ret

mich_recv:
    push ebx
    mov ebx, [esp + 8]
    mov eax, SYS_RECV
    int 0x80
    pop ebx
    ret

mich_recv_from:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov eax, SYS_RECV_FROM
    int 0x80
    pop ebx
    ret

mich_send_nb:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov eax, SYS_SEND_NB
    int 0x80
    pop ebx
    ret

mich_send_timeout:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov edx, [esp + 16]
    mov eax, SYS_SEND_TIMEOUT
    int 0x80
    pop ebx
    ret

mich_memfree:
    mov eax, SYS_MEMFREE
    int 0x80
    ret

mich_fork:
    mov eax, SYS_FORK
    int 0x80
    ret

mich_exec:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov eax, SYS_EXEC
    int 0x80
    pop ebx
    ret

mich_exit:
    mov ebx, [esp + 4]
    mov eax, SYS_EXIT
    int 0x80
.hang:
    jmp .hang

mich_wait:
    push ebx
    mov ebx, [esp + 8]
    mov eax, SYS_WAIT
    int 0x80
    pop ebx
    ret

mich_getpid:
    mov eax, SYS_GETPID
    int 0x80
    ret

mich_kill:
    push ebx
    mov ebx, [esp + 8]
    mov eax, SYS_KILL
    int 0x80
    pop ebx
    ret

mich_service_register:
    push ebx
    mov ebx, [esp + 8]
    mov eax, SYS_SERVICE_REGISTER
    int 0x80
    pop ebx
    ret

mich_service_lookup:
    push ebx
    mov ebx, [esp + 8]
    mov eax, SYS_SERVICE_LOOKUP
    int 0x80
    pop ebx
    ret

mich_cap_get:
    mov eax, SYS_CAP_GET
    int 0x80
    ret

mich_cap_drop:
    push ebx
    mov ebx, [esp + 8]
    mov eax, SYS_CAP_DROP
    int 0x80
    pop ebx
    ret

mich_cap_grant:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov eax, SYS_CAP_GRANT
    int 0x80
    pop ebx
    ret

mich_irq_register:
    push ebx
    mov ebx, [esp + 8]
    mov eax, SYS_IRQ_REG
    int 0x80
    pop ebx
    ret

mich_irq_grant:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov eax, SYS_IRQ_GRANT
    int 0x80
    pop ebx
    ret

mich_ioport_grant:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov edx, [esp + 16]
    mov eax, SYS_IOPORT_GRANT
    int 0x80
    pop ebx
    ret

mich_mmio_grant:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov edx, [esp + 16]
    mov eax, SYS_MMIO_GRANT
    int 0x80
    pop ebx
    ret

mich_mmio_map:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov eax, SYS_MMIO_MAP
    int 0x80
    pop ebx
    ret

mich_dma_grant:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov edx, [esp + 16]
    mov eax, SYS_DMA_GRANT
    int 0x80
    pop ebx
    ret

mich_dma_alloc:
    push ebx
    mov ebx, [esp + 8]
    mov ecx, [esp + 12]
    mov eax, SYS_DMA_ALLOC
    int 0x80
    pop ebx
    ret

mich_yield:
    mov eax, SYS_YIELD
    int 0x80
    ret
