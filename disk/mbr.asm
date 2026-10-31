; Minimal master boot record: loads the active partition's boot sector
        org 0x600
        cpu 286
start:  cli
        xor ax,ax
        mov ss,ax
        mov sp,0x7c00
        mov ds,ax
        mov es,ax
        sti
        cld
        mov si,0x7c00
        mov di,0x600
        mov cx,256
        rep movsw
        jmp 0:reloc
reloc:  mov si,0x7be
        mov cx,4
.find:  cmp byte [si],0x80
        je .found
        add si,16
        loop .find
        mov si,msg_noact
        jmp err
.found: mov bp,si
        mov di,5
.retry: mov dx,[bp]
        mov cx,[bp+2]
        mov bx,0x7c00
        mov ax,0x0201
        int 0x13
        jnc .ok
        xor ax,ax
        int 0x13
        dec di
        jnz .retry
        mov si,msg_err
        jmp err
.ok:    cmp word [0x7dfe],0xaa55
        jne .miss
        mov si,bp
        mov dl,0x80
        jmp 0:0x7c00
.miss:  mov si,msg_miss
err:    lodsb
        or al,al
        jz $
        mov ah,0x0e
        mov bx,7
        int 0x10
        jmp err
msg_noact: db "No active partition",0
msg_err:   db "Error loading OS",0
msg_miss:  db "Missing OS",0
        times 446-($-$$) db 0
