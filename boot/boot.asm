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
;
; FB_VIDMODE (bit 2 = 0x4) pide al bootloader que llene los campos
; framebuffer_* en multiboot_info. GRUB intenta poner el modo que pedimos
; (0,0,0,0 = "elige tu el mejor disponible").
;
; Como pedimos campos extra, el header crece de 12 a 48 bytes (el formato
; multiboot v1 con video info). Ver doc oficial.
MBALIGN     equ 1 << 0              ; align loaded modules on page boundaries
MEMINFO     equ 1 << 1              ; provide memory map
FB_VIDMODE  equ 1 << 2              ; request framebuffer (graphics mode)
MBFLAGS     equ MBALIGN | MEMINFO | FB_VIDMODE
MAGIC       equ 0x1BADB002          ; multiboot magic number
CHECKSUM    equ -(MAGIC + MBFLAGS)

section .multiboot
align 4
    ; --- header basico (12 bytes) ---
    dd MAGIC
    dd MBFLAGS
    dd CHECKSUM
    ; --- address fields (16 bytes, sin usar - los pone GRUB a 0) ---
    dd 0    ; header_addr
    dd 0    ; load_addr
    dd 0    ; load_end_addr
    dd 0    ; bss_end_addr
    dd 0    ; entry_addr
    ; --- video mode request (20 bytes) ---
    ; Pedimos 1366x768 32bpp (resolucion nativa de la HP Stream 14 y comun
    ; en otros ultrabooks). Si GRUB no la tiene disponible, da "el mejor
    ; disponible" automaticamente (no falla, simplemente da otra). Trinux
    ; se adapta a lo que reciba.
    dd 0       ; mode_type: 0 = linear graphics framebuffer (1 = EGA text)
    dd 1366    ; width pixels
    dd 768     ; height pixels
    dd 32      ; depth bits per pixel (XRGB8888)
    ; total: 48 bytes

; ---- Kernel stack (512 KiB — needs room for TCC programs' local arrays) ----
section .bss
align 16
stack_bottom:
    resb 524288
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
