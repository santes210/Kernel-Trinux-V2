; ============================================================================
; boot/kernel_entry.asm
;
; Alternative/auxiliary entry stub. The real entry point used by the linker is
; _start in boot/boot.asm (Multiboot). This file documents the classic
; "set stack, call kernel_main, hlt loop" pattern and exposes kernel_halt().
; ============================================================================

bits 32

section .text
global kernel_halt

; void kernel_halt(void) -> never returns
kernel_halt:
    cli
.loop:
    hlt
    jmp .loop
