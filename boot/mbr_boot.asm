; ============================================================================
; boot/mbr_boot.asm  -  REFERENCE ONLY (not part of `make run`)
;
; Classic 512-byte real-mode MBR boot sector. It sets up a GDT, enables A20,
; switches from 16-bit real mode to 32-bit protected mode and far-jumps into
; 32-bit code. This is provided for educational completeness; the actual build
; boots via Multiboot (boot/boot.asm) loaded by GRUB / `qemu -kernel`.
;
; Assemble standalone with:   nasm -f bin boot/mbr_boot.asm -o mbr.bin
; ============================================================================

bits 16
org 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; --- Enable A20 line (fast A20 via port 0x92) ---
    in al, 0x92
    or al, 2
    out 0x92, al

    ; --- Load GDT and enter protected mode ---
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1                ; set PE bit
    mov cr0, eax
    jmp CODE_SEG:protected_mode

bits 32
protected_mode:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; Print 'P' to VGA text buffer to prove we reached protected mode.
    mov byte [0xB8000], 'P'
    mov byte [0xB8001], 0x0F

.hang:
    hlt
    jmp .hang

; --- GDT ---
gdt_start:
    dq 0x0000000000000000        ; null descriptor
gdt_code:
    dw 0xFFFF                     ; limit low
    dw 0x0000                     ; base low
    db 0x00                       ; base mid
    db 10011010b                  ; access: present, ring0, code, exec/read
    db 11001111b                  ; flags + limit high
    db 0x00                       ; base high
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b                  ; access: present, ring0, data, read/write
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; --- Boot signature ---
times 510 - ($ - $$) db 0
dw 0xAA55
