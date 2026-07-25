BITS 16
org 0x7E00

%define BD_MAGIC   0x58424442
%define BLOB_LBA   32
%define BLOB_LOAD  0x10000
%define BDINFO     0x60000
%define BDSEG      0x6000
%define BMODS      0x60400
%define BSTRS      0x60480
%define VBEINFO    0x6000
%define MODEINFO   0x6400
%define MAX_MMAP   32

start2:
    cld
    mov [bootdrv], dl
    xor ax, ax
    mov ds, ax
    call serial_init
    mov si, msg_banner
    call serial_puts

    xor bx, bx
    mov ax, BDSEG
    mov es, ax
    mov byte [e820n], 0
    mov di, 0x0100
e820_loop:
    mov eax, 0x0000E820
    mov ecx, 24
    mov edx, 0x534D4150
    int 0x15
    jc e820_done
    cmp eax, 0x534D4150
    jne e820_done
    cmp ecx, 20
    jb e820_skip
    cmp dword [es:di + 8], 0
    je e820_skip
    inc byte [e820n]
    add di, 24
    cmp di, 0x0100 + MAX_MMAP * 24
    jae e820_done
e820_skip:
    test bx, bx
    jnz e820_loop
e820_done:
    xor ax, ax
    mov es, ax
    mov si, msg_e820
    call serial_puts

    mov ax, 0x4F00
    mov di, VBEINFO
    mov dword [di], 0x32454256
    int 0x10
    cmp ax, 0x004F
    jne vbe_fallback
    mov ax, [VBEINFO + 16]
    mov [listseg], ax
    mov si, [VBEINFO + 14]
scan_loop:
    mov ax, [listseg]
    mov es, ax
    mov cx, [es:si]
    xor ax, ax
    mov es, ax
    cmp cx, 0xFFFF
    je vbe_fallback
    mov [curmode], cx
    mov ax, 0x4F01
    mov di, MODEINFO
    int 0x10
    cmp ax, 0x004F
    jne scan_next
    mov ax, [MODEINFO]
    and ax, 0x0091
    cmp ax, 0x0091
    jne scan_next
    cmp word [MODEINFO + 18], 1280
    jne scan_next
    cmp word [MODEINFO + 20], 800
    jne scan_next
    cmp byte [MODEINFO + 25], 32
    jne scan_next
    cmp byte [MODEINFO + 27], 6
    jne scan_next
    call try_mode
    jmp after_vbe
scan_next:
    add si, 2
    jmp scan_loop
vbe_fallback:
    mov word [curmode], 0x0118
    call try_mode
after_vbe:

    mov byte [pkt], 0x10
    mov word [pkt + 2], 1
    mov word [pkt + 4], VBEINFO
    mov word [pkt + 6], 0
    mov dword [pkt + 8], BLOB_LBA
    mov dword [pkt + 12], 0
    call edd_read
    jc blob_fail
    cmp dword [VBEINFO], 0x424F4C42
    jne blob_fail
    mov eax, [VBEINFO + 4]
    mov [blob_rem], eax
    mov word [curseg], 0x1000
    mov dword [curlba], BLOB_LBA
blob_loop:
    mov eax, [blob_rem]
    test eax, eax
    jz blob_done
    cmp eax, 64
    jbe .small
    mov eax, 64
.small:
    mov [chunkn], eax
    mov word [pkt + 2], ax
    mov word [pkt + 4], 0
    mov ax, [curseg]
    mov word [pkt + 6], ax
    mov eax, [curlba]
    mov dword [pkt + 8], eax
    mov dword [pkt + 12], 0
    call edd_read
    jc blob_fail
    mov eax, [chunkn]
    sub [blob_rem], eax
    add [curlba], eax
    shl eax, 5
    add [curseg], ax
    jmp blob_loop
blob_done:
    mov si, msg_blob
    call serial_puts
    jmp a20_step
blob_fail:
    mov si, msg_blobfail
    call serial_puts
    jmp dead

try_mode:
    mov bx, [curmode]
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .bad
    mov cx, [curmode]
    mov ax, 0x4F01
    mov di, MODEINFO
    int 0x10
    cmp ax, 0x004F
    jne .bad
    mov ax, BDSEG
    mov es, ax
    mov eax, [MODEINFO + 40]
    es mov [8], eax
    xor eax, eax
    es mov [12], eax
    movzx eax, word [MODEINFO + 16]
    es mov [16], eax
    movzx eax, word [MODEINFO + 18]
    es mov [20], eax
    movzx eax, word [MODEINFO + 20]
    es mov [24], eax
    mov al, [MODEINFO + 25]
    es mov [28], al
    mov al, 1
    es mov [29], al
    xor ax, ax
    mov es, ax
    mov si, msg_vbe_ok
    call serial_puts
    ret
.bad:
    xor ax, ax
    mov es, ax
    mov si, msg_vbe_bad
    call serial_puts
    ret

edd_read:
    mov si, pkt
    mov ah, 0x42
    mov dl, [bootdrv]
    int 0x13
    ret

a20_step:
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al
    cli
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x0008:pm32

dead:
    cli
    hlt
    jmp dead

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

BITS 32
pm32:
    cld
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    lidt [idtr]
    mov esi, msg_pm
    call sputs32

    mov esi, [BLOB_LOAD + 8]
    add esi, BLOB_LOAD
    mov edi, 0x100000
    mov ecx, [BLOB_LOAD + 12]
    rep movsb

    mov edi, 0x100000
    add edi, [BLOB_LOAD + 228]
    mov ecx, [BLOB_LOAD + 232]
    xor eax, eax
    rep stosb

    mov ebp, [BLOB_LOAD + 224]
    mov eax, [BLOB_LOAD + 20]
    mov [modcount], eax
    xor ecx, ecx
mod_loop:
    cmp ecx, [modcount]
    jae mods_done
    mov eax, ecx
    imul eax, eax, 24
    lea esi, [BLOB_LOAD + 24 + eax + 8]
    mov edx, BSTRS
    mov ebx, ecx
    imul ebx, ebx, 24
    add edx, ebx
    push ecx
    push esi
    mov ecx, 6
    xor eax, eax
    mov edi, edx
    rep stosd
    pop esi
    mov edi, edx
    mov ecx, 4
    rep movsd
    pop ecx

    mov eax, ecx
    imul eax, eax, 24
    mov esi, [BLOB_LOAD + 24 + eax]
    add esi, BLOB_LOAD
    mov edx, [BLOB_LOAD + 24 + eax + 4]
    mov edi, ebp
    push ecx
    push edx
    mov ecx, edx
    rep movsb
    pop edx
    pop ecx

    mov eax, ecx
    imul eax, eax, 12
    mov ebx, BMODS
    add ebx, eax
    mov [ebx], ebp
    mov eax, ebp
    add eax, edx
    mov [ebx + 4], eax
    mov eax, ecx
    imul eax, eax, 24
    add eax, BSTRS
    mov [ebx + 8], eax

    add edx, 0xFFF
    and edx, 0xFFFFF000
    add ebp, edx
    inc ecx
    jmp mod_loop
mods_done:
    mov dword [BDINFO + 0], BD_MAGIC
    mov dword [BDINFO + 4], 1
    movzx eax, byte [e820n]
    mov [BDINFO + 32], eax
    mov dword [BDINFO + 36], BDINFO + 0x0100
    mov eax, [modcount]
    mov [BDINFO + 40], eax
    mov dword [BDINFO + 44], BMODS

    mov esi, msg_jump
    call sputs32
    cli
    mov edx, 0x100000
    add edx, [BLOB_LOAD + 16]
    push 0x0008
    push edx
    mov ebx, BDINFO
    mov eax, BD_MAGIC
    retf

pm_exc:
    mov [0x8700], eax
    mov [0x8704], ecx
    mov [0x8708], edx
    mov [0x870C], ebx
    mov [0x8710], ebp
    mov [0x8714], esi
    mov [0x8718], edi
    mov eax, [esp]
    mov [0x871C], eax
    mov eax, [esp + 4]
    mov [0x8720], eax
    mov eax, [esp + 8]
    mov [0x8724], eax
    mov al, 'E'
    call sputc32
exch:
    cli
    hlt
    jmp exch

sputc32:
    push eax
    push edx
    mov ah, al
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    pop edx
    pop eax
    ret

sputs32:
    push eax
    push esi
.loop:
    lodsb
    test al, al
    jz .done
    call sputc32
    jmp .loop
.done:
    pop esi
    pop eax
    ret

msg_banner:  db "bigdevboot v0.1 by bigdevboss", 13, 10, 0
msg_e820:    db "s2: e820 done", 13, 10, 0
msg_vbe_ok:  db "s2: vbe mode set", 13, 10, 0
msg_vbe_bad: db "s2: vbe FAIL", 13, 10, 0
msg_blob:    db "s2: blob loaded", 13, 10, 0
msg_blobfail: db "s2: blob FAIL", 13, 10, 0
msg_pm:      db "s2: pm32", 13, 10, 0
msg_jump:    db "s2: jumping to kernel", 13, 10, 0

bootdrv:  db 0
e820n:    db 0
curmode:  dw 0
listseg:  dw 0
curseg:   dw 0
align 4
curlba:   dd 0
blob_rem: dd 0
chunkn:   dd 0
modcount: dd 0

align 8
gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdtr:
    dw gdtr - gdt - 1
    dd gdt

align 8
idt:
%rep 32
    dw pm_exc
    dw 0x0008
    db 0
    db 0x8E
    dw 0
%endrep
idtr:
    dw idtr - idt - 1
    dd idt

align 4
pkt:      times 16 db 0
