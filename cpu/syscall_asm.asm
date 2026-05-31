; ============================================================================
; cpu/syscall_asm.asm  -  the int 0x80 entry stub and ring-3 entry helper.
;
; The syscall stub mirrors isr_common: it builds a registers_t frame on the
; kernel stack and calls syscall_handler(regs) in C. The handler reads the
; syscall number from eax and arguments from ebx/ecx/edx, and writes the
; return value back into regs->eax so the user sees it in eax after `iret`.
; ============================================================================

bits 32

extern syscall_handler
global syscall_stub
global enter_usermode
global usermode_save_and_enter
global usermode_exit_resume

; --- save kernel context, then drop to ring 3 -------------------------------
; int usermode_save_and_enter(uint32_t entry, uint32_t user_stack,
;                             uint32_t *save_esp);
; Saves callee-saved regs + the kernel ESP into *save_esp, then enters ring 3.
; When the user calls SYS_EXIT, usermode_exit_resume() restores this and makes
; THIS function return with eax = exit code.
usermode_save_and_enter:
    push ebx
    push esi
    push edi
    push ebp
    ; stack now: [esp]=ebp +4=edi +8=esi +12=ebx +16=retaddr
    ;            +20=arg1(entry) +24=arg2(user_stack) +28=arg3(save_esp)
    mov eax, [esp + 28]        ; save_esp pointer (3rd arg)
    mov [eax], esp             ; remember kernel stack here
    ; fall through into the ring-3 entry using args 1 and 2
    mov eax, [esp + 20]        ; entry
    mov edx, [esp + 24]        ; user stack
    ; build iret frame (same as enter_usermode)
    cli
    mov cx, 0x23
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx
    push dword 0x23
    push edx
    pushfd
    pop ecx
    or ecx, 0x200
    push ecx
    push dword 0x1b
    push eax
    iret

; void usermode_exit_resume(uint32_t saved_esp, int code);
; Restores the kernel stack saved above and returns `code` from
; usermode_save_and_enter (as if it had just returned).
usermode_exit_resume:
    mov ecx, [esp + 4]    ; saved kernel esp
    mov eax, [esp + 8]    ; exit code -> return value
    ; reload kernel data segments (we may be coming from the ring0 trap frame,
    ; which already has them, but be safe)
    mov dx, 0x10
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx
    mov esp, ecx          ; back onto the saved kernel stack
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret                   ; returns to caller of usermode_save_and_enter

; --- int 0x80 handler -------------------------------------------------------
syscall_stub:
    ; No error code for software interrupts. Push a dummy err_code and a
    ; pseudo "int_no" of 0x80 so the frame matches registers_t.
    push dword 0          ; err_code
    push dword 0x80       ; int_no
    pusha                 ; edi,esi,ebp,esp,ebx,edx,ecx,eax
    mov ax, ds
    push eax              ; save data segment

    mov ax, 0x10          ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp              ; -> registers_t *regs
    call syscall_handler
    add esp, 4

    pop eax               ; restore data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa                  ; eax now holds the syscall return value
    add esp, 8            ; pop int_no + err_code
    iret

; --- drop into ring 3 -------------------------------------------------------
; void enter_usermode(uint32_t entry, uint32_t user_stack);
; Builds an iret frame that switches CS/SS to the ring-3 selectors and jumps
; to `entry` with `user_stack` as ESP. User selectors: code 0x1b, data 0x23
; (GDT entries 3 and 4 with RPL 3).
enter_usermode:
    mov eax, [esp + 4]    ; entry point
    mov edx, [esp + 8]    ; user stack top

    cli
    mov cx, 0x23          ; user data selector (RPL 3)
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    push dword 0x23       ; SS  (user data)
    push edx              ; ESP (user stack)
    pushfd                ; EFLAGS
    pop ecx
    or ecx, 0x200         ; set IF so interrupts are enabled in user mode
    push ecx
    push dword 0x1b       ; CS  (user code, RPL 3)
    push eax              ; EIP (entry)
    iret                  ; pops EIP,CS,EFLAGS,ESP,SS -> we are now in ring 3
