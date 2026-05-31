; cpu/idt_flush.asm  -  load the IDT register.
bits 32
section .text
global idt_flush

; void idt_flush(uint32_t idt_ptr_addr)
idt_flush:
    mov eax, [esp + 4]
    lidt [eax]
    ret
