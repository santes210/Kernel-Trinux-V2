; ============================================================================
; boot/boot.asm  -  Multiboot header + kernel entry (32-bit protected mode)
;
; We boot via the Multiboot v1 protocol (GRUB / `qemu-system-i386 -kernel`).
; GRUB/QEMU already drop us into 32-bit protected mode with a flat GDT, so we
; only need to: declare the multiboot header, set up our own stack, set up a
; proper GDT (boot/gdt.asm), and jump into kernel_main() in C.
;
; A real-mode 512-byte MBR bootloader is provided separately in
; boot/mbr_boot.asm as a documented reference (not used by `make run`).
; ============================================================================

bits 32

; ---- Multiboot v1 header constants ----
MBALIGN     equ 1 << 0              ; align loaded modules on page boundaries
MEMINFO     equ 1 << 1             ; provide memory map
MBFLAGS     equ MBALIGN | MEMINFO
MAGIC       equ 0x1BADB002          ; multiboot magic number
CHECKSUM    equ -(MAGIC + MBFLAGS)

section .multiboot
align 4
    dd MAGIC
    dd MBFLAGS
    dd CHECKSUM

; ---- Kernel stack (16 KiB) ----
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; ---- Entry point ----
section .text
global _start
extern kernel_main
extern gdt_install        ; from boot/gdt.c

_start:
    cli
    mov esp, stack_top     ; set up the stack

    ; GRUB passes: EAX = multiboot magic, EBX = pointer to multiboot info.
    ; Preserve them across gdt_install().
    push ebx               ; save multiboot info pointer
    push eax               ; save multiboot magic

    call gdt_install       ; load our own GDT + reload segment registers

    ; Re-fetch the saved args (gdt_install is cdecl, callee-preserves nothing
    ; we rely on, so reload from stack) and pass to kernel_main(magic, info).
    ; Stack currently: [esp]=magic, [esp+4]=ebx
    ; kernel_main(uint32_t magic, uint32_t mb_info_addr)
    call kernel_main       ; args already on the stack in the right order

    ; If kernel_main ever returns, halt forever.
.hang:
    cli
    hlt
    jmp .hang
