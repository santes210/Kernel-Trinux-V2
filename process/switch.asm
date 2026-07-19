; ============================================================================
; process/switch.asm  -  cooperative context switch with address space switch.
;
; void context_switch(context_t *old, context_t *new, uint32_t new_page_dir);
; Saves callee-saved registers + flags + return EIP into *old, then loads
; them from *new, switches CR3 to new_page_dir, and resumes.
; context_t layout (see process.h):
;   uint32_t esp, ebp, ebx, esi, edi, eflags, eip;
; ============================================================================

bits 32
section .text
global context_switch

context_switch:
    mov eax, [esp + 4]      ; old context pointer (may be NULL)
    mov edx, [esp + 8]      ; new context pointer
    mov ecx, [esp + 12]     ; new page directory physical address

    test eax, eax
    jz .load                ; if old == NULL, just load new

    ; Save current state into *old
    mov [eax + 0],  esp     ; esp
    mov [eax + 4],  ebp     ; ebp
    mov [eax + 8],  ebx     ; ebx
    mov [eax + 12], esi     ; esi
    mov [eax + 16], edi     ; edi
    pushfd
    pop ebx
    mov [eax + 20], ebx     ; eflags
    mov ebx, [esp]          ; return address -> eip
    mov [eax + 24], ebx

.load:
    ; Switch address space FIRST (before loading new stack/registers)
    ; This ensures the new process's page tables are active when we load its stack
    test ecx, ecx
    jz .no_cr3_switch
    mov cr3, ecx            ; switch to new address space

.no_cr3_switch:
    ; Load state from *new
    mov esp, [edx + 0]
    mov ebp, [edx + 4]
    mov ebx, [edx + 8]
    mov esi, [edx + 12]
    mov edi, [edx + 16]
    mov ebx, [edx + 20]
    push ebx
    popfd
    mov ebx, [edx + 24]     ; new eip
    mov [esp], ebx          ; replace return address
    ret
