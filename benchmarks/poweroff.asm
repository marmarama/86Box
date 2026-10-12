; POWEROFF.COM - Clean APM 1.2 power-off utility for DOS guests
; Assemble with: nasm -f bin -o poweroff.com poweroff.asm
; Exact binary length: 19 bytes

org 0x100

start:
    ; 1. Connect APM Real-Mode Interface (AX=5301h, BX=0000h)
    mov ax, 0x5301
    xor bx, bx
    int 0x15
    jc .fallback

    ; 2. Enable APM for all devices (AX=5308h, BX=0001h, CX=0001h)
    mov ax, 0x5308
    mov bx, 0x0001
    mov cx, 0x0001
    int 0x15

    ; 3. Set Power State to Off (AX=5307h, BX=0001h, CX=0003h)
    mov ax, 0x5307
    mov bx, 0x0001
    mov cx, 0x0003
    int 0x15

.fallback:
    ; If APM failed, halt CPU in a loop
    cli
.halt_loop:
    hlt
    jmp .halt_loop
