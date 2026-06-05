; cpu/elf_jmp.asm
;
; Tiny setjmp/longjmp pair used by elf_exec() to give SYS_EXIT (and
; usermode_fault_kill) a way to unwind the kernel stack all the way back
; out of `fn()` without context-switching, without calling schedule(),
; and without depending on the user-stack return address (which is
; garbage because the program runs as a plain function call).
;
; The buffer layout matches `elf_jmp_t` in cpu/syscall.c:
;     offset 0  : valid     (uint32)   not touched here
;     offset 4  : esp       (uint32)
;     offset 8  : ebp       (uint32)
;     offset 12 : ebx       (uint32)
;     offset 16 : esi       (uint32)
;     offset 20 : edi       (uint32)
;     offset 24 : eip       (uint32)
;     offset 28 : exit_code (int32)    not touched here

bits 32

global elf_jmp_setjmp
global elf_jmp_longjmp

; int elf_jmp_setjmp(elf_jmp_t *dst);
;   Returns 0 the first time, non-zero when restored via longjmp.
elf_jmp_setjmp:
    mov eax, [esp + 4]           ; eax = dst
    mov edx, [esp]               ; edx = return address (caller's EIP)

    ; Save callee-saved + esp/ebp/eip.  esp value stored is what the
    ; caller's esp will be right after the call returns — i.e. esp + 4.
    lea ecx, [esp + 4]
    mov [eax + 4],  ecx          ; saved esp
    mov [eax + 8],  ebp          ; saved ebp
    mov [eax + 12], ebx
    mov [eax + 16], esi
    mov [eax + 20], edi
    mov [eax + 24], edx          ; saved eip

    xor eax, eax                 ; return 0 from the "save" path
    ret

; void elf_jmp_longjmp(elf_jmp_t *src);
;   Restores the saved frame and jumps back, returning ~0 from setjmp.
elf_jmp_longjmp:
    mov eax, [esp + 4]           ; eax = src

    mov ebx, [eax + 12]
    mov esi, [eax + 16]
    mov edi, [eax + 20]
    mov ebp, [eax + 8]
    mov esp, [eax + 4]
    mov ecx, [eax + 24]          ; saved eip

    mov eax, 1                   ; return value of the original setjmp call
    jmp ecx
