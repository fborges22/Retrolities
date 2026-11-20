; El Torito no-emulation test loader (4 virtual sectors loaded by Polaris).
; Uses only INT 13h on the drive number it was given: asks for the
; El Torito status (AH=4Bh), reads its own second CD block (AH=42h) and
; prints the text stored there.
        bits 16
        org 7C00h
start:
        cli
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 7C00h
        sti
        mov [drive], dl
        mov ax, 0003h
        int 10h
        mov si, m1
        call print
        mov al, [drive]
        call hex8
        mov si, m2
        call print
        mov ax, 4B01h                   ; El Torito: get status
        mov dl, [drive]
        mov si, spec
        int 13h
        jc .nospec
        mov si, m3
        call print
        mov ax, [spec + 6]
        call hex16
        mov ax, [spec + 4]
        call hex16
        mov si, crlf
        call print
.nospec:
        mov ax, [spec + 4]              ; boot image LBA; message follows it
        add ax, 1
        mov [dap + 8], ax
        mov ah, 42h
        mov dl, [drive]
        mov si, dap
        int 13h
        jc .err
        mov si, 8000h
        call print
        jmp short .halt
.err:   mov si, m4
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
m1:     db "Polaris El Torito no-emulation test", 13, 10, 13, 10
        db "  Booted from CD drive ", 0
m2:     db "h through Polaris' resident ATAPI driver", 13, 10, 0
m3:     db "  INT 13h AH=4B01h: boot image at CD block ", 0
m4:     db "  INT 13h AH=42h read FAILED", 13, 10, 0
crlf:   db 13, 10, 0
drive:  db 0
dap:    db 10h, 0
        dw 1, 8000h, 0
        dd 0, 0
spec:   times 20 db 0
        times 2048 - ($ - $$) db 0
; second CD block: read back with INT 13h AH=42h
        db "  INT 13h AH=42h read 2048-byte block: OK", 13, 10, 13, 10
        db "  The CD was booted without any BIOS CD-ROM support.", 13, 10, 0
        times 4096 - ($ - $$) db 0
