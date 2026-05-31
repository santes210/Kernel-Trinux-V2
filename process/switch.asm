; ============================================================================
; process/switch.asm  -  cooperative context switch.
;
; void context_switch(context_t *old, context_t *new);
; Saves callee-saved registers + flags + return EIP into *old, then loads
; them from *new and resumes. context_t layout (see process.h):
;   uint32_t esp, ebp, ebx, esi, edi, eflags, eip;
; ============================================================================

bits 32
section .text
global context_switch

context_switch:
    mov eax, [esp + 4]      ; old context pointer (may be NULL)
    mov edx, [esp + 8]      ; new context pointer

    test eax, eax
    jz .load                ; if old == NULL, just load new

    ; Save current state into *old
    mov [eax + 0],  esp     ; esp
    mov [eax + 4],  ebp     ; ebp
    mov [eax + 8],  ebx     ; ebx
    mov [eax + 12], esi     ; esi
    mov [eax + 16], edi     ; edi
    pushfd
    pop ecx
    mov [eax + 20], ecx     ; eflags
    mov ecx, [esp]          ; return address -> eip
    mov [eax + 24], ecx

.load:
    ; Load state from *new
    mov esp, [edx + 0]
    mov ebp, [edx + 4]
    mov ebx, [edx + 8]
    mov esi, [edx + 12]
    mov edi, [edx + 16]
    mov ecx, [edx + 20]
    push ecx
    popfd
    mov ecx, [edx + 24]     ; new eip
    mov [esp], ecx          ; replace return address
    ret
