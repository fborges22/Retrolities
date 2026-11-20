; Polaris boot manager - stage 1
;
; One 512-byte sector that serves both as a hard disk master boot record and
; as a floppy boot sector.  Bytes 03h..3Dh are left for a FAT BPB (the
; installer copies the floppy's BPB there); 1B8h..1FDh keep the disk
; signature and partition table of a hard disk.  Stage 1 loads stage 2 to
; 1000:0100 and jumps to it with DL = boot drive.
;
; Build: nasm -f bin stage1.asm -o stage1.bin

        bits 16
        org 7C00h
        cpu 8086

STAGE2_SEG  equ 1000h

start:
        jmp short main
        nop
bpb:    times 3Eh - ($ - $$) db 0       ; FAT BPB / EBPB (floppy installs)

main:
        cli
        xor ax, ax
        mov ss, ax
        mov sp, 7C00h
        mov ds, ax
        mov es, ax
        sti
        cld
        jmp 0:.norm                     ; some BIOSes enter at 07C0:0000
.norm:
        mov [drive], dl

        ; --- choose the read method: EDD packets or CHS ---------------------
        test dl, 80h
        jz .floppy
        mov ah, 41h
        mov bx, 55AAh
        int 13h
        jc .hdgeo
        cmp bx, 0AA55h
        jne .hdgeo
        test cl, 1
        jz .hdgeo
        mov byte [useedd], 1
.hdgeo:
        mov ah, 08h
        mov dl, [drive]
        push es
        int 13h
        pop es
        jc .defgeo
        and cl, 3Fh
        jz .defgeo
        mov [spt], cl
        inc dh
        mov [heads], dh
        jmp short .load
.floppy:
        mov ax, [bpb + 18h - 3]         ; BPB sectors per track
        test ax, ax
        jz .defgeo
        cmp ax, 63
        ja .defgeo
        mov [spt], al
        mov ax, [bpb + 1Ah - 3]         ; BPB heads
        test ax, ax
        jz .defgeo
        mov [heads], al
        jmp short .load
.defgeo:                                ; keep 18/2 (floppy) or 63/255
        test byte [drive], 80h
        jz .load
        mov byte [spt], 63
        mov byte [heads], 255

        ; --- load stage 2, one sector at a time -------------------------------
.load:
        mov si, info_lba
        lodsw
        mov bp, ax                      ; bp = current LBA
        lodsw
        mov di, ax                      ; di = sectors left
        mov bx, STAGE2_SEG + 10h        ; destination segment (offset 0)
.next:
        mov cx, 4                       ; attempts
.retry:
        push cx
        call read_sector
        pop cx
        jnc .ok
        xor ax, ax                      ; reset the drive and try again
        mov dl, [drive]
        int 13h
        loop .retry
        jmp short error
.ok:
        add bx, 20h
        inc bp
        dec di
        jnz .next

        ; --- check the stage-2 signature and enter it ------------------------
        mov ax, STAGE2_SEG
        mov es, ax
        mov di, 103h
        mov si, sig
        mov cx, 4
        repe cmpsb
        jne error
        mov dl, [drive]
        mov ds, ax
        cli
        mov ss, ax
        mov sp, 0FFFEh
        sti
        jmp STAGE2_SEG:0100h

error:
        mov si, msg
.print:
        lodsb
        test al, al
        jz .wait
        mov ah, 0Eh
        mov bx, 7
        int 10h
        jmp short .print
.wait:
        xor ax, ax
        int 16h
        int 18h
        jmp short .wait

; read LBA bp into bx:0000; returns CF on error
read_sector:
        mov dl, [drive]
        cmp byte [useedd], 0
        je .chs
        xor ax, ax
        push ax
        push ax                         ; LBA bits 32..63
        push ax
        push bp                         ; LBA bits 0..31
        push bx
        push ax                         ; buffer bx:0000
        mov al, 1
        push ax                         ; 1 sector
        mov al, 10h
        push ax                         ; packet size
        mov si, sp
        mov ah, 42h
        int 13h
        lea sp, [si + 10h]              ; drop the packet, keep CF
        ret
.chs:
        mov ax, bp
        xor dx, dx
        mov cl, [spt]
        xor ch, ch
        div cx                          ; ax = track, dx = sector - 1
        mov cl, dl
        inc cl
        xor dx, dx
        push cx
        mov cl, [heads]
        xor ch, ch
        div cx                          ; ax = cylinder, dx = head
        pop cx
        mov dh, dl
        mov ch, al
        ror ah, 1
        ror ah, 1
        or cl, ah                       ; cylinder bits 8-9
        mov dl, [drive]
        mov es, bx
        push bx
        xor bx, bx
        mov ax, 0201h
        int 13h
        pop bx
        ret

msg:    db 13, 10, "Polaris: stage 2 error", 0
sig:    db "POLA"
drive:  db 0
useedd: db 0
spt:    db 18
heads:  db 2

        times 1A0h - ($ - $$) db 0
info:   db "PLRS"                       ; struct pl_info
        db 1, 0                         ; version
info_lba:
        dw 5                            ; stage2_lba
        dw 0                            ; stage2_count (set by installer)
        dw 1                            ; config_lba
        db 0                            ; media: 0 hard disk, 1 floppy
        db 0
        times 1FEh - ($ - $$) db 0
        dw 0AA55h
