; Test payload for the bare-metal RV32 build.
; Flat 32-bit (cpui386_reset_pm), loaded at 0x1000.
; Every emitted value is COMPUTED, so it cannot come from anywhere but
; real emulated execution -- constants would prove nothing.
bits 32
org 0x1000
start:
    mov edx, 0x80              ; POST-code port
    mov ecx, 5
.loop:
    mov eax, ecx
    imul eax, eax              ; eax = ecx^2   (exercises multiply)
    out dx, eax                ; expect 25,16,9,4,1
    loop .loop                 ; exercises LOOP + ECX decrement

    ; memory round-trip through phys_mem
    mov eax, 0xA5A5A5A5
    mov [0x2000], eax
    xor eax, eax               ; clobber, so a stale reg can't fake it
    mov eax, [0x2000]
    out dx, eax                ; expect A5A5A5A5

    ; byte/word access sizes
    mov byte [0x2010], 0x42
    movzx eax, byte [0x2010]
    out dx, eax                ; expect 42

    mov eax, 0xDEADBEEF
    out dx, eax                ; end marker
.halt:
    jmp .halt
