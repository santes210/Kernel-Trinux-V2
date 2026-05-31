; ============================================================================
; boot/gdt.asm  -  Load the GDT register and reload segment selectors.
; ============================================================================

bits 32

section .text
global gdt_flush

; void gdt_flush(uint32_t gdt_ptr_addr)
gdt_flush:
    mov eax, [esp + 4]    ; argument: address of the gdt_ptr struct
    lgdt [eax]            ; load the new GDT

    mov ax, 0x10          ; 0x10 = data segment selector (3rd entry)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.flush       ; 0x08 = code segment selector; far jump reloads CS
.flush:
    ret
