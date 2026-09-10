BITS 16
org 0x7C00

start:
    cli
    cld
    mov al, 0xFF
    out 0xA1, al
    out 0x21, al
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [bootdrv], dl

    call serial_init
    mov si, msg_s1
    call serial_puts

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [bootdrv]
    int 0x13
    jc sfail
    cmp bx, 0xAA55
    jne sfail

    mov word [pkt + 2], 16
    mov word [pkt + 4], 0x7E00
    mov word [pkt + 6], 0
    mov dword [pkt + 8], 1
    mov dword [pkt + 12], 0
    mov si, pkt
    mov ah, 0x42
    mov dl, [bootdrv]
    int 0x13
    jc sfail

    mov si, msg_ok
    call serial_puts

    mov dl, [bootdrv]
    jmp 0x0000:0x7E00

sfail:
    mov si, msg_fail
    call serial_puts
    cli
    hlt
    jmp $

serial_init:
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 0x01
    out dx, al
    mov dx, 0x3F9
    mov al, 0x00
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al
    ret

serial_putc:
    push ax
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    ret

serial_puts:
    lodsb
    test al, al
    jz .done
    call serial_putc
    jmp serial_puts
.done:
    ret

msg_s1:   db "bigdevboot s1 alive", 13, 10, 0
msg_ok:   db "s1: stage2 loaded, jumping", 13, 10, 0
msg_fail: db "s1: FAIL", 13, 10, 0
bootdrv:  db 0
align 4
pkt:      times 16 db 0

times 510 - ($ - $$) db 0
dw 0xAA55
