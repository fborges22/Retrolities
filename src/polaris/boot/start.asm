; Polaris stage 2 - startup and runtime support (tiny model image).
;
; Stage 1 loads the linked .COM-format image at 1000:0100 and jumps to it
; with DL = boot drive.

        cpu 386
        bits 16

segment _TEXT public class=CODE align=16 use16
segment CONST public class=DATA align=2 use16
segment CONST2 public class=DATA align=2 use16
segment _DATA public class=DATA align=16 use16
segment _BSS public class=BSS align=2 use16
segment ZZEND public class=BSS align=2 use16
group DGROUP _TEXT CONST CONST2 _DATA _BSS ZZEND

extern main_
global _boot_drive, _resident_bin, _resident_end, _stage2_end
global __U4M, __I4M, __U4D, __I4D, _cstart_

segment _TEXT
        resb 100h
..start:
_cstart_:
        jmp near entry
        db "POLARIS2"                   ; checked by stage 1 and installers
entry:
        cli
        mov ax, cs
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0FFFEh
        sti
        cld
        mov [_boot_drive], dl
        mov di, bss_start
        mov cx, bss_end
        sub cx, di
        xor al, al
        rep stosb
        call main_
.halt:
        sti
        hlt
        jmp short .halt

; 32-bit multiply/divide helpers called by Open Watcom 16-bit code
__U4M:
__I4M:
        push ecx
        shl edx, 16
        mov dx, ax
        shl ecx, 16
        mov cx, bx
        imul edx, ecx
        mov ax, dx
        shr edx, 16
        pop ecx
        ret
__U4D:
        shl edx, 16
        mov dx, ax
        mov eax, edx
        shl ecx, 16
        mov cx, bx
        xor edx, edx
        div ecx
        jmp short divout
__I4D:
        shl edx, 16
        mov dx, ax
        mov eax, edx
        shl ecx, 16
        mov cx, bx
        cdq
        idiv ecx
divout:
        mov ebx, edx
        mov ecx, edx
        shr ecx, 16
        mov edx, eax
        shr edx, 16
        ret

segment _DATA
_boot_drive:    db 0
                align 16
_resident_bin:  incbin "resident.bin"
_resident_end:

segment _BSS
bss_start:

segment ZZEND
bss_end:
_stage2_end:
