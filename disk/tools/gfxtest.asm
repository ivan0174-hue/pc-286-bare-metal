        org 100h
        cpu 286
; ---- mode 13h: 256 vertical colour bars + diagonal
        mov ax,13h
        int 10h
        mov ax,0A000h
        mov es,ax
        xor di,di
        mov dx,200
.y13:   mov cx,320
        xor bl,bl
.x13:   mov ax,320
        sub ax,cx
        mov [es:di],al
        inc di
        loop .x13
        dec dx
        jnz .y13
        call key
; ---- mode 12h: 16 horizontal bands using write mode 2
        mov ax,12h
        int 10h
        mov dx,3CEh
        mov ax,0205h
        out dx,ax
        xor di,di
        xor bl,bl
.band:  mov cx,30*80
.bl:    mov al,[es:di]
        mov [es:di],bl
        inc di
        loop .bl
        inc bl
        cmp bl,16
        jb .band
        mov ax,0005h
        out dx,ax
        call key
; ---- VESA 101h 640x480x256 with bank switching
        mov ax,4F02h
        mov bx,101h
        int 10h
        cmp ax,004Fh
        jne .novesa
        xor dx,dx          ; bank
.bank:  push dx
        mov ax,4F05h
        xor bx,bx
        int 10h
        pop dx
        xor di,di
.px:    mov ax,di
        ; colour = (linear offset / 640) & 0FFh approx: use bank*102 + di>>9
        mov al,dl
        shl al,5
        mov bx,di
        shr bx,11
        add al,bl
        mov [es:di],al
        inc di
        jnz .px
        inc dx
        cmp dx,5
        jb .bank
        call key
.novesa:
        mov ax,3
        int 10h
        mov ax,4C00h
        int 21h
key:    xor ax,ax
        int 16h
        ret
