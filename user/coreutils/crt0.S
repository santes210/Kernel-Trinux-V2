/* crt0.S — entry stub para programas Trinux ring-3.
 * El kernel deja en el user stack:
 *   [esp+0]=dummy ret, [esp+4]=argc, [esp+8]=argv
 * Llamamos main(argc,argv) y luego SYS_EXIT(rc). */
    .intel_syntax noprefix
    .global _start
    .text
_start:
    mov  eax, [esp + 4]
    mov  edx, [esp + 8]
    push edx
    push eax
    call main
    add  esp, 8
    mov  ebx, eax
    mov  eax, 1          /* SYS_EXIT */
    int  0x80
1:  jmp  1b
