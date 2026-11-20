; Polaris - real-mode helpers shared by stage 2 and the DOS installer.
; OMF object for Open Watcom (small/tiny model, near code).

        cpu 586
        bits 16

segment _TEXT public class=CODE align=1 use16

global pl_intr, pl_boot_jump, pl_cpu_class, pl_cpuid

; void pl_intr(AL = interrupt, SI -> rmregs in DGROUP (== SS))
; rmregs: eax ebx ecx edx esi edi (32-bit), ds es flags bp (16-bit)
pl_intr:
        push bp
        push ds
        push es
        pushad
        mov [cs:.op + 1], al
        jmp short .go                   ; flush the prefetch queue
.go:
        push si
        mov eax, [si]
        mov ebx, [si + 4]
        mov ecx, [si + 8]
        mov edx, [si + 12]
        mov edi, [si + 20]
        mov es, [si + 26]
        push word [si + 24]
        mov bp, [si + 30]
        mov esi, [si + 16]
        pop ds
.op:    int 0
        pushf
        push ds
        push esi
        mov bp, sp                      ; [bp] esi, [bp+4] ds, [bp+6] flags
        mov si, [bp + 8]                ; rmregs
        mov [ss:si], eax
        mov [ss:si + 4], ebx
        mov [ss:si + 8], ecx
        mov [ss:si + 12], edx
        mov [ss:si + 20], edi
        mov eax, [bp]
        mov [ss:si + 16], eax
        mov ax, [bp + 4]
        mov [ss:si + 24], ax
        mov [ss:si + 26], es
        mov ax, [bp + 6]
        mov [ss:si + 28], ax
        add sp, 10
        popad
        pop es
        pop ds
        pop bp
        ret

; void pl_boot_jump(AL = drive, SI = entry offset (segment 0), BX:CX = target)
pl_boot_jump:
        cli
        mov dl, al
        xor dh, dh
        xor ax, ax
        mov ss, ax
        mov sp, 7C00h
        mov ds, ax
        mov es, ax
        push bx
        push cx
        mov bp, si
        xor bx, bx
        xor cx, cx
        xor di, di
        xor ax, ax
        sti
        retf

; AL = 3 (386), 4 (486 without CPUID), 5 (CPUID available)
pl_cpu_class:
        pushfd
        pop eax
        mov ecx, eax
        xor eax, 40000h                 ; AC
        push eax
        popfd
        pushfd
        pop eax
        push ecx
        popfd
        xor eax, ecx
        mov dl, 3
        test eax, 40000h
        jz .done
        mov eax, ecx
        xor eax, 200000h                ; ID
        push eax
        popfd
        pushfd
        pop eax
        push ecx
        popfd
        xor eax, ecx
        mov dl, 4
        test eax, 200000h
        jz .done
        mov dl, 5
.done:
        mov al, dl
        ret

; void pl_cpuid(DX:AX = leaf, SI -> u32[4] eax ebx ecx edx)
pl_cpuid:
        push di
        shl edx, 16
        mov dx, ax
        mov eax, edx
        xor ecx, ecx
        cpuid
        mov [si], eax
        mov [si + 4], ebx
        mov [si + 8], ecx
        mov [si + 12], edx
        pop di
        ret
