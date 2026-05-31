; ============================================================================
; cpu/isr_asm.asm  -  CPU exception stubs (ISR 0-31).
; Each stub pushes a dummy error code (if the CPU doesn't), the interrupt
; number, then jumps to the common handler which builds a register frame and
; calls isr_handler() in C.
; ============================================================================

bits 32

extern isr_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push dword 0          ; dummy error code
    push dword %1         ; interrupt number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push dword %1         ; interrupt number (CPU already pushed err code)
    jmp isr_common
%endmacro

; Exceptions with error codes: 8, 10, 11, 12, 13, 14, 17
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

isr_common:
    pusha                 ; edi, esi, ebp, esp, ebx, edx, ecx, eax
    mov ax, ds
    push eax              ; save data segment

    mov ax, 0x10          ; load kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp              ; pointer to registers_t -> isr_handler(regs)
    call isr_handler
    add esp, 4

    pop eax               ; restore data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8            ; pop int_no and err_code
    sti
    iret
