; Polaris stage 2 - self-unpacking header.
;
; Stage 1 loads this stub, followed by the PLZ-packed boot menu, at
; 1000:0100 (DS = 1000h, DL = boot drive).  It unpacks the menu to
; 2000:0100 and jumps there.  Format: tools/plz.py.

        bits 16
        cpu 8086
        org 100h

        jmp short go
        nop
        db "POLARIS2"                   ; checked by stage 1 and the installers

go:
        cld
        push dx
        mov si, packed
        mov ax, 2000h
        mov es, ax
        mov di, 100h
        mov bh, 80h                     ; empty bit buffer (sentinel)
        call gamma
literals:
        rep movsb
        call getbit                     ; after literals
        jc newoff
        call gamma                      ; match with the previous offset
copy:
        push si
        push ds
        mov si, di
        sub si, [cs:lastoff]
        push es
        pop ds
        rep movsb
        pop ds
        pop si
        call getbit                     ; after a match
        jc newoff
        call gamma
        jmp short literals
newoff:
        call gamma                      ; high byte of the offset + 1
        cmp cx, 256
        je done
        dec cx
        mov ah, cl
        lodsb
        inc ax
        mov [cs:lastoff], ax
        call gamma
        inc cx
        jmp short copy
done:
        pop dx
        jmp 2000h:0100h

getbit:
        add bh, bh
        jnz .ok
        mov bh, [si]
        inc si
        adc bh, bh
.ok:    ret

gamma:
        xor bp, bp
.count: call getbit
        jc .value
        inc bp
        jmp short .count
.value: mov cx, 1
.read:  test bp, bp
        jz .done
        call getbit
        adc cx, cx
        dec bp
        jmp short .read
.done:  ret

lastoff: dw 0
packed:
