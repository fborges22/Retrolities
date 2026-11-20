; Demo partition boot sector for Polaris screenshots: shows which partition
; was started (Polaris passes DL and DS:SI = partition entry), then waits.
; The message is patched in by mkdemo.py at offset 0x1A0 (NUL terminated).
        bits 16
        org 7C00h
        jmp short start
        nop
        times 0x5A - ($ - $$) db 0      ; room for any BPB (FAT32 size)
start:
        cli
        xor ax, ax
        mov ss, ax
        mov sp, 7C00h
        sti
        mov [cs:drive], dl
        mov ax, [si + 8]                ; partition LBA from DS:SI
        mov [cs:plba], ax
        mov ax, [si + 10]
        mov [cs:plba + 2], ax
        push cs
        pop ds
        mov ax, 0003h
        int 10h
        mov si, 7C00h + 1A0h
        call print
        mov si, info
        call print
        mov al, [drive]
        call hex8
        mov si, info2
        call print
        mov ax, [plba + 2]
        call hex16
        mov ax, [plba]
        call hex16
        mov si, crlf
        call print
.halt:  hlt
        jmp short .halt
hex16:  push ax
        mov al, ah
        call hex8
        pop ax
hex8:   push ax
        shr al, 4
        call nib
        pop ax
nib:    and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     mov ah, 0Eh
        int 10h
        ret
print:  lodsb
        test al, al
        jz .d
        mov ah, 0Eh
        mov bx, 7
        int 10h
        jmp short print
.d:     ret
info:   db 13, 10, 13, 10, "  Started by Polaris: boot drive ", 0
info2:  db "h, partition LBA ", 0
crlf:   db 13, 10, 0
drive:  db 0
plba:   dd 0
        times 1A0h - ($ - $$) db 0
        times 510 - ($ - $$) db 0
        dw 0AA55h
