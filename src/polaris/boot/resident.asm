; Polaris boot manager - resident INT 13h extension
;
; Copied by stage 2 to the top of conventional memory just before an
; operating system is started.  It provides:
;
;   * BIOS drive remapping (boot from the second disk, a SCSI disk, ...)
;   * a virtual BIOS drive backed by an ATAPI device through Polaris' own
;     IDE/ATAPI PIO driver, so CD-ROMs, DVDs, ZIP, LS-120, MO and PD drives
;     boot on machines whose BIOS knows nothing about them:
;       mode 1  El Torito "no emulation" (2048-byte sectors, EDD calls)
;       mode 2  floppy emulation (1.2/1.44/2.88 MB image, or LS-120/ZIP
;               presented as drive 00h)
;       mode 3  hard disk emulation (CD image or a ZIP disk as drive 80h)
;
; The image is position independent: everything is addressed through CS
; with ORG 0.  The block below (offset 8) is filled in by stage 2;
; resident.h in the C sources mirrors it.
;
; Build: nasm -f bin resident.asm -o resident.bin

        bits 16
        cpu 386
        org 0

        jmp near isr                    ; INT 13h entry
        db 0
        db 'PR13'                       ; offset 4: signature

; ---- parameter block (offset 8) ---------------------------------------------
old13:      dd 0                        ; 08 original INT 13h vector
map:        times 8 db 0                ; 0C up to 4 (from, to) pairs
mapcount:   db 0                        ; 14
vdrive:     db 0FFh                     ; 15 virtual drive, FFh = none
mode:       db 0                        ; 16 0 none, 1 no-emu, 2 floppy, 3 hdd
unitsel:    db 0A0h                     ; 17 A0h master, B0h slave
iobase:     dw 1F0h                     ; 18 command block
ctlbase:    dw 3F6h                     ; 1A control block (alt status)
blksize:    dw 2048                     ; 1C device block size
shift:      db 2                        ; 1E log2(blksize / 512)
hdadj:      db 0                        ; 1F added to DL by AH=08h on disks
start:      dd 0                        ; 20 image start (device blocks)
sectors:    dd 0                        ; 24 image length (512-byte sectors)
cyls:       dw 80                       ; 28 emulated geometry
heads:      db 2                        ; 2A
spt:        db 18                       ; 2B
ftype:      db 4                        ; 2C AH=08h BL drive type
media:      db 2                        ; 2D El Torito media type
loadseg:    dw 07C0h                    ; 2E
loadcnt:    dw 4                        ; 30
catlba:     dd 0                        ; 32 boot catalog
cached:     dd 0FFFFFFFFh               ; 36 block in the cache
memkb:      dw 0                        ; 3A KB taken from the BIOS
laststat:   db 0                        ; 3C
controller: db 0                        ; 3D controller index (spec packet)
            dw 0                        ; 3E
; -----------------------------------------------------------------------------

isr:
        cmp dl, [cs:vdrive]
        je virtual
        cmp ah, 4Bh                     ; El Torito status, "any CD" form
        jne .notcd
        cmp dl, 7Fh
        jne .notcd
        cmp byte [cs:vdrive], 0FFh
        jne virtual
.notcd:
        push ax
        push si
        push cx
        call lookup                     ; CF set: DL is remapped to AL
        pop cx
        pop si
        jc remap
        cmp ah, 08h                     ; the disk count must include an
        jne .chain                      ; emulated hard disk
        test dl, 80h
        jz .chain
        cmp byte [cs:hdadj], 0
        je .chain
        mov al, dl
        jmp short remap
.chain:
        pop ax
        jmp far [cs:old13]

; call the BIOS with DL = AL; the caller's AX is on the stack
remap:
        push bp
        mov bp, sp                      ; [bp+2] caller's AX
        push dx                         ; [bp-2] caller's DX
        mov dl, al
        mov ax, [bp+2]
        push ax                         ; [bp-4] caller's AX (function)
        pushf
        call far [cs:old13]
        mov [bp+2], ax                  ; BIOS result, popped below
        pushf
        mov al, [bp-3]                  ; function
        cmp al, 08h
        je .count
        cmp al, 15h
        je .keep
        mov dl, [bp-2]                  ; give the caller its drive back
        jmp short .keep
.count:
        test byte [bp-2], 80h
        jz .keep
        add dl, [cs:hdadj]
.keep:
        popf
        mov sp, bp
        pop bp
        pop ax
        retf 2

; look up DL in the map; CF=1 and AL=new drive when found (AL kept otherwise)
lookup:
        mov cl, [cs:mapcount]
        xor ch, ch
        jcxz .no
        mov si, map
.loop:
        cmp dl, [cs:si]
        je .yes
        add si, 2
        loop .loop
.no:
        clc
        ret
.yes:
        mov al, [cs:si+1]
        stc
        ret

; =============================================================================
; virtual drive
; =============================================================================
R_DI    equ 0
R_SI    equ 4
R_BX    equ 16
R_DX    equ 20
R_CX    equ 24
R_AX    equ 28
R_ES    equ 32
R_DS    equ 34
R_FL    equ 40

virtual:
        sti
        push ds
        push es
        pushad
        mov bp, sp
        cld
        push cs
        pop ds
        mov al, ah
        cmp al, 00h
        je ok
        cmp al, 01h
        je getstat
        cmp al, 02h
        je chs_read
        cmp al, 03h
        je wprot
        cmp al, 04h
        je verify
        cmp al, 08h
        je params
        cmp al, 0Ch
        je ok
        cmp al, 0Dh
        je ok
        cmp al, 10h
        je ok
        cmp al, 11h
        je ok
        cmp al, 15h
        je gettype
        cmp al, 16h
        je ok
        cmp al, 41h
        je extcheck
        cmp al, 42h
        je ext_read
        cmp al, 43h
        je wprot
        cmp al, 44h
        je ok
        cmp al, 45h
        je ok
        cmp al, 47h
        je ok
        cmp al, 48h
        je extparams
        cmp al, 4Bh
        je eltorito
        cmp al, 4Eh
        je ok
bad:    mov ah, 01h
        jmp short fail
wprot:  mov ah, 03h
        jmp short fail
notfound:
        mov ah, 04h
fail:
        mov [laststat], ah
        mov [bp+R_AX+1], ah
        or byte [bp+R_FL], 1
        jmp short leave
ok:
        xor ah, ah
        mov [laststat], ah
        mov [bp+R_AX+1], ah
        and byte [bp+R_FL], 0FEh
leave:
        popad
        pop es
        pop ds
        iret

getstat:
        mov ah, [laststat]
        mov [bp+R_AX+1], ah
        and byte [bp+R_FL], 0FEh
        jmp short leave

verify:
        cmp byte [mode], 1
        je bad
        jmp short ok

; ---- AH=02h CHS read (emulation modes) ---------------------------------------
chs_read:
        cmp byte [mode], 1
        je bad
        ; lba = (cyl * heads + head) * spt + sector - 1
        mov ax, [bp+R_CX]               ; CH = cyl low, CL = sector | cyl hi
        movzx ebx, al
        and bl, 3Fh                     ; sector
        jz notfound
        xor dx, dx
        mov dl, al
        shl dx, 2
        and dx, 300h
        mov dl, ah                      ; dx = cylinder
        movzx eax, dx
        movzx ecx, byte [heads]
        mul ecx
        movzx ecx, byte [bp+R_DX+1]     ; head
        add eax, ecx
        movzx ecx, byte [spt]
        mul ecx
        dec bx
        add eax, ebx                    ; eax = 512-byte sector
        movzx cx, byte [bp+R_AX]        ; count
        jcxz notfound
        mov di, [bp+R_BX]
        mov es, [bp+R_ES]
        xor dx, dx                      ; sectors done
.next:
        push eax
        push cx
        call read512
        pop cx
        pop eax
        jc .err
        inc dx
        inc eax
        call advance512
        loop .next
        mov [bp+R_AX], dl               ; AL = sectors read
        jmp ok
.err:
        mov [bp+R_AX], dl
        jmp fail

; es:di += 512, normalised
advance512:
        mov bx, es
        add bx, 20h
        mov es, bx
        ret

; ---- AH=08h drive parameters -------------------------------------------------
params:
        cmp byte [mode], 1
        je bad
        mov ax, [cyls]
        dec ax
        mov ch, al
        mov cl, ah
        shl cl, 6
        or cl, [spt]
        mov dh, [heads]
        dec dh
        mov [bp+R_CX], cx
        cmp byte [mode], 2
        jne .hd
        mov dl, 1                       ; one floppy drive (ours)
        mov [bp+R_DX], dx
        movzx bx, byte [ftype]
        mov [bp+R_BX], bx
        xor ax, ax
        mov es, ax
        mov ax, [es:78h]                ; INT 1Eh diskette parameter table
        mov [bp+R_DI], ax
        mov ax, [es:7Ah]
        mov [bp+R_ES], ax
        mov word [bp+R_AX], 0
        jmp ok
.hd:
        mov ax, 40h
        mov es, ax
        mov dl, [es:75h]                ; disks, including the emulated one
        mov [bp+R_DX], dx
        jmp ok

; ---- AH=15h drive type --------------------------------------------------------
gettype:
        cmp byte [mode], 2
        jne .hd
        mov byte [bp+R_AX+1], 01h       ; floppy, no change line
        and byte [bp+R_FL], 0FEh
        jmp leave
.hd:
        mov ax, [sectors]
        mov [bp+R_DX], ax
        mov ax, [sectors+2]
        mov [bp+R_CX], ax
        mov byte [bp+R_AX+1], 03h
        and byte [bp+R_FL], 0FEh
        jmp leave

; ---- AH=41h EDD installation check -----------------------------------------
extcheck:
        cmp word [bp+R_BX], 55AAh
        jne bad
        cmp byte [mode], 2
        je bad
        mov word [bp+R_BX], 0AA55h
        mov word [bp+R_CX], 0001h       ; packet access subset
        mov byte [bp+R_AX+1], 21h       ; EDD 1.1
        and byte [bp+R_FL], 0FEh
        jmp leave

; ---- AH=42h extended read ---------------------------------------------------
ext_read:
        push ds
        mov ds, [bp+R_DS]
        mov si, [bp+R_SI]
        mov cx, [si+2]                  ; count
        les di, [si+4]                  ; buffer
        mov eax, [si+8]                 ; LBA (low 32 bits)
        pop ds
        jcxz .done
        xor dx, dx
        cmp byte [mode], 1
        je .noemu
.sec:
        push eax
        push cx
        call read512
        pop cx
        pop eax
        jc .err
        inc dx
        inc eax
        call advance512
        loop .sec
.done:
        jmp ok
.noemu:
        push eax
        push cx
        push dx
        call fetch
        pop dx
        pop cx
        pop eax
        jc .err
        push cx
        mov si, cache
        mov cx, [blksize]
        shr cx, 1
        rep movsw
        pop cx
        mov bx, es                      ; normalise es:di
        mov si, di
        shr si, 4
        add bx, si
        mov es, bx
        and di, 0Fh
        inc dx
        inc eax
        loop .noemu
        jmp ok
.err:
        push ds
        mov ds, [bp+R_DS]
        mov si, [bp+R_SI]
        mov [si+2], dx                  ; blocks actually transferred
        pop ds
        jmp fail

; ---- AH=48h extended parameters ---------------------------------------------
extparams:
        cmp byte [mode], 2
        je bad
        push ds
        mov es, [bp+R_DS]
        mov di, [bp+R_SI]
        mov ax, 26
        stosw                           ; buffer size
        mov ax, 0074h                   ; removable, change line, lockable
        cmp byte [mode], 3
        jne .flags
        mov ax, 0002h                   ; CHS information valid
.flags:
        stosw
        movzx eax, word [cyls]
        stosd
        movzx eax, byte [heads]
        stosd
        movzx eax, byte [spt]
        stosd
        mov eax, [sectors]
        cmp byte [mode], 1
        jne .total
        mov cl, [shift]
        shr eax, cl                     ; device blocks
.total:
        stosd
        xor eax, eax
        stosd
        mov ax, 512
        cmp byte [mode], 1
        jne .bps
        mov ax, [blksize]
.bps:
        stosw
        pop ds
        jmp ok

; ---- AH=4Bh El Torito ---------------------------------------------------------
eltorito:
        mov al, [bp+R_AX]
        cmp al, 2
        jae bad
        mov es, [bp+R_DS]
        mov di, [bp+R_SI]
        mov al, 13h
        stosb
        mov al, [media]
        stosb
        mov al, [vdrive]
        stosb
        mov al, [controller]
        stosb
        mov eax, [start]
        stosd
        movzx ax, byte [unitsel]        ; device specification: bit0 = slave
        and al, 10h
        shr ax, 4
        stosw
        xor ax, ax
        stosw                           ; user buffer segment
        mov ax, [loadseg]
        stosw
        mov ax, [loadcnt]
        stosw
        mov ax, [cyls]
        dec ax
        mov dl, al                      ; cylinder low
        shl ah, 6
        or ah, [spt]
        mov al, ah
        stosb                           ; sector + cylinder high
        mov al, dl
        stosb
        mov al, [heads]
        dec al
        stosb
        jmp ok

; =============================================================================
; block access
; =============================================================================

; read 512-byte image sector EAX to es:di; CF on error
read512:
        cmp eax, [sectors]
        jae .range
        push edx
        push es
        push di
        mov cl, [shift]
        mov ebx, eax
        shr eax, cl
        add eax, [start]
        push bx
        call fetch
        pop bx
        pop di
        pop es
        pop edx
        jc .out
        mov cl, [shift]
        mov ax, 1
        shl ax, cl
        dec ax
        and bx, ax
        shl bx, 9
        lea si, [cache + bx]
        push di
        mov cx, 256
        rep movsw
        pop di
        clc
.out:   ret
.range: stc
        mov ah, 04h
        ret

; make sure device block EAX is in the cache; CF on error (AH = status)
fetch:
        cmp eax, [cached]
        je .hit
        mov dword [cached], 0FFFFFFFFh
        mov cx, 3
.try:
        push cx
        push eax
        call atapi_read
        pop eax
        pop cx
        jnc .done
        call atapi_reset
        loop .try
        mov ah, 20h                     ; controller failure
        stc
        ret
.done:
        mov [cached], eax
.hit:
        clc
        ret

; ---- ATAPI ------------------------------------------------------------------

; wait until BSY clears; returns status in AL; CF on timeout
wait_ready:
        push es
        push bx
        push ecx
        mov bx, 40h
        mov es, bx
        mov ecx, [es:6Ch]
        add ecx, 18 * 10                ; ten seconds (spin-up)
.loop:
        mov dx, [ctlbase]
        in al, dx                       ; alternate status
        test al, 80h
        jz .ready
        cmp ecx, [es:6Ch]
        jae .loop
        stc
        jmp short .out
.ready:
        mov dx, [iobase]
        add dx, 7
        in al, dx                       ; status, clears a pending IRQ
        clc
.out:
        pop ecx
        pop bx
        pop es
        ret

; ATAPI DEVICE RESET after a failed command
atapi_reset:
        push cx
        mov dx, [iobase]
        add dx, 6
        mov al, [unitsel]
        out dx, al
        call io_delay
        mov dx, [iobase]
        add dx, 7
        mov al, 08h
        out dx, al
        call io_delay
        call wait_ready
        call irq_on
        pop cx
        ret

io_delay:
        push cx
        mov dx, [ctlbase]
        mov cx, 4
.d:     in al, dx
        loop .d
        pop cx
        ret

; read one block (EAX) into the cache with READ(10); CF on error
atapi_read:
        push es
        push di
        mov ebx, eax
        mov dx, [ctlbase]
        mov al, 02h                     ; nIEN: we poll
        out dx, al
        call wait_ready
        jc .fail
        mov dx, [iobase]
        add dx, 6
        mov al, [unitsel]
        out dx, al
        call io_delay
        call wait_ready
        jc .fail
        mov dx, [iobase]
        inc dx
        xor al, al
        out dx, al                      ; features: PIO
        add dx, 3
        mov ax, [blksize]
        out dx, al                      ; byte count limit
        inc dx
        mov al, ah
        out dx, al
        add dx, 2
        mov al, 0A0h                    ; PACKET
        out dx, al
        call io_delay
        call wait_ready
        jc .fail
        test al, 01h
        jnz .fail
        test al, 08h
        jz .fail
        ; READ(10) packet: 28 00 lba(4, big endian) 00 00 01 00 00 00
        mov dx, [iobase]
        mov ax, 0028h
        out dx, ax
        mov eax, ebx                    ; LBA, big endian, without BSWAP
        shr eax, 16
        xchg al, ah
        out dx, ax
        mov ax, bx
        xchg al, ah
        out dx, ax
        xor ax, ax
        out dx, ax                      ; reserved, count high
        mov ax, 0001h                   ; count low (=1), control
        out dx, ax
        xor ax, ax
        out dx, ax
        ; data phase(s)
        push cs
        pop es
        mov di, cache
        mov si, [blksize]               ; bytes still expected
.data:
        call io_delay
        call wait_ready
        jc .fail
        test al, 01h
        jnz .fail
        test al, 08h
        jz .end
        mov dx, [iobase]
        add dx, 4
        in al, dx
        mov cl, al
        inc dx
        in al, dx
        mov ch, al                      ; cx = bytes in this phase
        shr cx, 1
        jcxz .data
        mov dx, [iobase]
.word:
        in ax, dx
        cmp si, 2
        jb .drop
        stosw
        sub si, 2
.drop:  loop .word
        jmp short .data
.end:
        test si, si
        jnz .fail
        call irq_on
        pop di
        pop es
        clc
        ret
.fail:
        call irq_on
        pop di
        pop es
        stc
        ret

; clear a pending INTRQ and give the interrupt back to the BIOS
irq_on:
        mov dx, [iobase]
        add dx, 7
        in al, dx
        mov dx, [ctlbase]
        xor al, al
        out dx, al
        ret

        align 16
cache:                                  ; 2048-byte block cache follows
