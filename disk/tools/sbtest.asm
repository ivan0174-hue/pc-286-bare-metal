        org 100h
        cpu 286
        mov dx,msg_hdr
        call puts
; --- DSP reset
        mov dx,226h
        mov al,1
        out dx,al
        mov cx,100
.d1:    in al,dx
        loop .d1
        xor al,al
        out dx,al
        mov cx,0FFFFh
.w1:    mov dx,22Eh
        in al,dx
        test al,80h
        jnz .rd
        loop .w1
        jmp fail_reset
.rd:    mov dx,22Ah
        in al,dx
        cmp al,0AAh
        jne fail_reset
        mov dx,msg_reset
        call puts
; --- version
        mov al,0E1h
        call dspw
        call dspr
        add al,'0'
        mov [ver],al
        call dspr
        mov ah,al
        shr al,4
        add al,'0'
        mov [ver+2],al
        and ah,0Fh
        add ah,'0'
        mov [ver+3],ah
        mov dx,msg_ver
        call puts
; --- build a square wave buffer
        mov di,buf
        mov cx,4000
        xor bx,bx
.fb:    mov al,40h
        test bx,10h
        jz .lo
        mov al,0C0h
.lo:    stosb
        inc bx
        loop .fb
; --- hook IRQ7 (int 0Fh)
        cli
        xor ax,ax
        mov es,ax
        mov word [es:0Fh*4],irq7
        mov [es:0Fh*4+2],cs
        push cs
        pop es
        in al,21h
        and al,7Fh
        out 21h,al
        sti
; --- DMA ch1: compute 20-bit physical address
        mov ax,cs
        mov dx,ax
        shl ax,4
        shr dx,12
        add ax,buf
        adc dx,0
        mov bx,ax
        mov al,5
        out 0Ah,al        ; mask ch1
        xor al,al
        out 0Ch,al
        mov al,49h        ; single, inc, read (mem->dev), ch1
        out 0Bh,al
        mov al,bl
        out 02h,al
        mov al,bh
        out 02h,al
        mov al,dl
        out 83h,al
        mov ax,4000-1
        out 03h,al
        mov al,ah
        out 03h,al
        mov al,1
        out 0Ah,al        ; unmask
        mov al,0D1h
        call dspw         ; speaker on
        mov al,40h
        call dspw
        mov al,256-91     ; ~11 kHz
        call dspw
        mov al,14h
        call dspw
        mov al,(4000-1) & 0FFh
        call dspw
        mov al,(4000-1) >> 8
        call dspw
; wait for irq (max ~2 s)
        xor si,si
.wi:    cmp byte [got_irq],0
        jne .irqok
        mov cx,1000
.wl:    loop .wl
        inc si
        cmp si,30000
        jb .wi
        mov dx,msg_noirq
        call puts
        jmp opl
.irqok: mov dx,msg_irq
        call puts
; --- OPL (AdLib) detection
opl:    mov ah,4
        mov al,60h
        call oplw
        mov al,80h
        call oplw
        mov dx,388h
        in al,dx
        mov bl,al
        mov ah,2
        mov al,0FFh
        call oplw
        mov ah,4
        mov al,21h
        call oplw
        mov cx,2000
.ow:    in al,dx
        loop .ow
        in al,dx
        mov bh,al
        mov ah,4
        mov al,60h
        call oplw
        mov al,80h
        call oplw
        and bl,0E0h
        and bh,0E0h
        cmp bl,0
        jne .nof
        cmp bh,0C0h
        jne .nof
        mov dx,msg_opl
        call puts
        jmp done
.nof:   mov dx,msg_noopl
        call puts
done:   mov ax,4C00h
        int 21h
fail_reset:
        mov dx,msg_fail
        call puts
        mov ax,4C01h
        int 21h

irq7:   push ax
        push dx
        mov dx,22Eh
        in al,dx
        mov byte [cs:got_irq],1
        mov al,20h
        out 20h,al
        pop dx
        pop ax
        iret

oplw:   push ax
        mov dx,388h
        xchg al,ah
        out dx,al
        mov cx,6
.a:     in al,dx
        loop .a
        inc dx
        mov al,ah
        out dx,al
        dec dx
        mov cx,35
.b:     in al,dx
        loop .b
        pop ax
        ret
dspw:   push ax
        mov dx,22Ch
.w:     in al,dx
        test al,80h
        jnz .w
        pop ax
        out dx,al
        ret
dspr:   mov dx,22Eh
.w:     in al,dx
        test al,80h
        jz .w
        mov dx,22Ah
        in al,dx
        ret
puts:   mov ah,9
        int 21h
        ret
got_irq db 0
msg_hdr   db "SBTEST: Sound Blaster test",13,10,"$"
msg_reset db "DSP reset OK",13,10,"$"
msg_ver   db "DSP version "
ver       db "0.00",13,10,"$"
msg_irq   db "DMA playback + IRQ 7 OK",13,10,"$"
msg_noirq db "DMA/IRQ FAILED",13,10,"$"
msg_opl   db "OPL2 (AdLib) detected OK",13,10,"$"
msg_noopl db "OPL2 NOT detected",13,10,"$"
msg_fail  db "DSP reset FAILED",13,10,"$"
buf:
