; boot/longmode.asm — Detección y transición a Long Mode (x86_64).
;
; CAMBIO #11: infraestructura para soporte de 64 bits.
;
; Este módulo proporciona:
;   1. longmode_check()   — detecta si la CPU soporta Long Mode via CPUID.
;   2. longmode_enter()   — configura las estructuras de 64 bits (PML4, PDPT,
;                           PD) y salta al código de 64 bits.
;
; Diseño del layout de memoria para Long Mode:
;   0x1000  PML4  (Page Map Level 4) — 1 entrada
;   0x2000  PDPT  (Page Directory Pointer Table) — 1 entrada
;   0x3000  PD    (Page Directory) — 512 entradas × 2 MiB = 1 GiB identity-map
;
; El identity-map de 1 GiB (usando páginas de 2 MiB) cubre el kernel
; que vive en 1 MiB y toda la RAM necesaria para arrancar.
;
; NOTA: este archivo NO forma parte del build actual (32-bit) de Trinux.
; Es la base para una futura versión 64-bit. Se compila por separado:
;   nasm -f elf64 boot/longmode.asm -o boot/longmode.o
;   (requiere un kernel_main_64 y un linker script para 64 bits)

bits 32

global longmode_check
global longmode_enter

; ---- longmode_check() → eax = 1 si OK, 0 si no soportado ----
longmode_check:
    ; Verificar que CPUID soporta el bit de Extended Functions
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_longmode

    ; Verificar el bit de Long Mode (bit 29 de EDX en leaf 0x80000001)
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)
    jz .no_longmode

    mov eax, 1
    ret

.no_longmode:
    xor eax, eax
    ret

; ---- longmode_enter(entry64) — salta a código de 64 bits ----
; Argumento (en stack, cdecl): dirección de 64 bits del entry point.
;
; Pasos:
;   1. Deshabilitar interrupciones
;   2. Construir PML4, PDPT, PD en 0x1000/0x2000/0x3000
;   3. Cargar CR3 con la dirección de PML4
;   4. Habilitar PAE (CR4.PAE = bit 5)
;   5. Activar Long Mode en EFER MSR (MSR 0xC0000080, bit 8)
;   6. Activar paginación (CR0.PG = bit 31) — esto entra en Long Mode
;   7. Cargar una GDT de 64 bits y hacer un far-jump al entry point
longmode_enter:
    cli

    ; ---- Limpiar las páginas de tablas ----
    xor eax, eax
    mov edi, 0x1000
    mov ecx, 3 * 4096 / 4   ; 3 tablas × 4096 bytes / 4 bytes por stosd
    rep stosd

    ; ---- PML4[0] → PDPT en 0x2000 ----
    mov dword [0x1000], 0x2003   ; PRESENT | WRITE | dirección 0x2000
    mov dword [0x1004], 0        ; bits altos (0 para < 4 GiB)

    ; ---- PDPT[0] → PD en 0x3000 ----
    mov dword [0x2000], 0x3003
    mov dword [0x2004], 0

    ; ---- PD: 512 entradas × 2 MiB = 1 GiB identity-map ----
    ; Cada entrada: (i × 2MiB) | PAGE_SIZE (bit 7) | PRESENT | WRITE
    mov edi, 0x3000
    xor eax, eax
    mov ecx, 512
.fill_pd:
    mov dword [edi],     eax
    or  dword [edi],     0x83   ; PRESENT | WRITE | PS (2MiB page)
    mov dword [edi + 4], 0
    add eax, 0x200000           ; + 2 MiB
    add edi, 8
    loop .fill_pd

    ; ---- Cargar CR3 con PML4 ----
    mov eax, 0x1000
    mov cr3, eax

    ; ---- Habilitar PAE (CR4.PAE = bit 5) ----
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; ---- Activar Long Mode en EFER MSR ----
    mov ecx, 0xC0000080         ; EFER MSR
    rdmsr
    or  eax, (1 << 8)           ; LME (Long Mode Enable)
    wrmsr

    ; ---- Activar paginación → entra en Compatibility Mode (32-bit CS) ----
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 0)   ; PG | PE
    mov cr0, eax

    ; ---- Cargar GDT de 64 bits ----
    lgdt [gdt64_ptr]

    ; ---- Far jump al segmento de código de 64 bits ----
    ; Esto activa el CS de 64 bits y entra en Long Mode real.
    ; El entry point viene como argumento en [esp + 4].
    ; Cargamos la dirección en eax (será extendida a rax en 64 bits).
    mov eax, [esp + 4]
    jmp 0x08:.bits64

bits 64
.bits64:
    ; Ahora estamos en 64 bits.
    ; Recargar segmentos de datos con el selector nulo (en 64 bits son opcionales)
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Llamar al entry point de 64 bits (en rax)
    ; Extender eax a rax (zero-extend ya lo hace la asignación anterior)
    call rax

    ; Si regresa, halt
.hang64:
    cli
    hlt
    jmp .hang64

; ---- GDT de 64 bits (mínima: null + code64) ----
bits 32
align 8
gdt64:
    ; Descriptor nulo
    dq 0
    ; Descriptor de código 64-bit: base=0, limit=0, L=1, P=1, DPL=0, type=exec/read
    dq 0x00AF9A000000FFFF   ; L=1 (64-bit), P=1, DPL=0, C/D=code, readable

gdt64_ptr:
    dw $ - gdt64 - 1        ; límite
    dd gdt64                ; base (32 bits — suficiente mientras PML4 sea < 4 GiB)
