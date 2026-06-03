bits 32
global kernel_thread_exit
extern process_exit
extern schedule

kernel_thread_exit:
    push dword 0    ; exit code 0
    call process_exit
    add esp, 4
.hang:
    call schedule
    hlt
    jmp .hang
