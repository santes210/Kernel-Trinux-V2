/* kernel/elf.c  -  ELF32 executable loader.
 *
 * Reads an ELF32 executable from the VFS, loads its PT_LOAD segments into
 * memory, and jumps to the entry point in ring 3 (userspace).
 *
 * The user program communicates with the kernel ONLY via int 0x80 syscalls
 * (same as the built-in `usertest` demo).  When it calls SYS_EXIT, control
 * returns here.
 *
 * Memory layout for user programs:
 *   - Code/data loaded at the addresses specified in the ELF program headers
 *     (typically starting at 0x08048000 for a standard i386 executable, but
 *     we also support lower addresses within the identity-mapped 1 GiB).
 *   - A user stack is allocated at a fixed high address within the
 *     identity-mapped region.
 *   - Each process has its own address space (page directory) created by
 *     process_create(). The ELF is loaded into that process's address space.
 *     The kernel's identity-mapped page tables are shared for the first 1 GiB
 *     (shallow copy), so loading via virtual addresses works in both contexts.
 */
#include "elf.h"
#include "../fs/vfs.h"
#include "../mm/kheap.h"
#include "../mm/vmm.h"
#include "../cpu/syscall.h"
#include "../process/process.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/serial.h"

/* Where we allocate the user stack (within identity-mapped 1 GiB). */
/* Cada nivel de anidamiento usa un user stack en VA distinta para no
 * pisar el stack del padre. Aún sin address space por proceso real, esto
 * basta para shell->hijo->nieto (3 niveles). Cada nivel reserva 64 KB.
 *
 * Nivel 0 (shell)    : 0x0F000000  (256 KiB hacia abajo)
 * Nivel 1 (cmd)      : 0x0EE00000
 * Nivel 2 (sub-cmd)  : 0x0EC00000
 *
 * Las VAs estan dentro del identity-map de 1 GiB, asi que el CPU las ve
 * en cualquier momento. La proteccion de aislamiento depende del bit User
 * de las paginas (ya activado en vmm_init para el identity map). */
#define USER_STACK_SIZE     0x10000   /* 64 KiB por nivel */
#define USER_STACK_TOP_L0   0x0F000000U
#define USER_STACK_TOP_L1   0x0EE00000U
#define USER_STACK_TOP_L2   0x0EC00000U
#define USER_STACK_TOP_L3   0x0EA00000U   /* por si acaso */
#define USER_STACK_TOP_FALLBACK 0x0E800000U

/* Rebase del segundo nivel de recursion (cuando un ELF ring-3 hace
 * SPAWN de otro ELF): cargamos al hijo a OFFSET arriba de donde linkeo,
 * para no pisar el codigo del padre que vive en 0x08048000.
 *
 * Es una "reubicación a mano" pre-padding del p_vaddr: cada PT_LOAD se
 * carga en (p_vaddr + g_elf_rebase) y el entry point se ajusta.
 *
 * Limitacion: SOLO funciona si el ELF es position-independent O si el
 * tamaño total cabe sin que se referencien direcciones absolutas.
 * Nuestros coreutils estan enlazados a 0x08048000 fijo, asi que un
 * rebase romperia sus accesos absolutos.
 *
 * Alternativa mas simple y correcta: GUARDAR Y RESTAURAR la region
 * 0x08048000..0x08100000 (768 KB) alrededor del spawn anidado.
 * Eso es lo que hacemos abajo, con un kmalloc temporal. */
static uint8_t *g_padre_backup;
static uint32_t g_padre_backup_size;
#define PADRE_SAVE_BASE  0x08048000
#define PADRE_SAVE_SIZE  0x000B8000   /* 736 KB -- cubre code+data+bss del shell */

/* Validate an ELF32 header. */
static bool elf_validate(const elf32_ehdr_t *hdr)
{
    if (hdr->e_magic != ELF_MAGIC) {
        serial_write("[elf] bad magic\n");
        return false;
    }
    if (hdr->e_class != 1) {   /* must be 32-bit */
        serial_write("[elf] not 32-bit\n");
        return false;
    }
    if (hdr->e_data != 1) {    /* must be little-endian */
        serial_write("[elf] not little-endian\n");
        return false;
    }
    if (hdr->e_type != ET_EXEC) {
        serial_write("[elf] not ET_EXEC\n");
        return false;
    }
    if (hdr->e_machine != EM_386) {
        serial_write("[elf] not i386\n");
        return false;
    }
    return true;
}

int elf_exec(const char *path, vfs_node_t *cwd)
{
    return elf_exec_argv(path, cwd, 0, NULL);
}

/* Profundidad de anidamiento de elf_exec: 0 = primer ELF, 1 = ELF lanzado
 * desde otro ELF (caso del shell ring-3 que hace SPAWN).  Necesario para
 * saber cuando respaldar el codigo del padre. */
static int g_nest = 0;

/* Forward declaration for the inner function that does the actual work */
static int elf_exec_argv_inner(const char *path, vfs_node_t *cwd, int argc, char **argv, process_t *proc);

/* Wrapper that handles nesting, backup/restore, and process creation */
int elf_exec_argv(const char *path, vfs_node_t *cwd, int argc, char **argv)
{
    /* Si vamos a entrar a un nivel >=1, guardamos la region de codigo
     * del padre porque el hijo se cargara en la misma VA. */
    uint8_t *backup = NULL;
    char   **argv_copy = NULL;
    int      did_save = 0;
    if (g_nest >= 1) {
        backup = (uint8_t *)kmalloc(PADRE_SAVE_SIZE);
        if (backup) {
            memcpy(backup, (void *)PADRE_SAVE_BASE, PADRE_SAVE_SIZE);
            did_save = 1;
        }
        /* Tambien argv vive en el espacio del padre (su stack/scratch).
         * Lo copiamos a kheap antes de que el ELF nuevo lo pise. */
        if (argc > 0 && argv) {
            uint32_t total = 0;
            for (int i = 0; i < argc; i++) {
                int sl = 0; while (argv[i][sl]) sl++;
                total += sl + 1;
            }
            uint8_t *blob = (uint8_t *)kmalloc(total + (argc+1) * sizeof(char *));
            if (blob) {
                argv_copy = (char **)blob;
                char *str = (char *)(blob + (argc+1) * sizeof(char *));
                for (int i = 0; i < argc; i++) {
                    argv_copy[i] = str;
                    int j = 0;
                    while (argv[i][j]) { *str++ = argv[i][j]; j++; }
                    *str++ = 0;
                }
                argv_copy[argc] = NULL;
                argv = argv_copy;
            }
        }
    }
    g_nest++;

    /* Create the process FIRST (which creates its address space) */
    char name[32];
    const char *slash = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p + 1;
    strncpy(name, slash, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    process_t *proc = process_create(name, NULL);
    process_t *prev_current = process_get_current();
    int rc = -1;
    if (proc) {
        proc->state = PROC_RUNNING;
        process_set_current(proc);
        rc = elf_exec_argv_inner(path, cwd, argc, argv, proc);
    }

    g_nest--;
    if (did_save && backup) {
        memcpy((void *)PADRE_SAVE_BASE, backup, PADRE_SAVE_SIZE);
        kfree(backup);
    }
    if (argv_copy) kfree(argv_copy);

    /* Restore previous current */
    if (prev_current) process_set_current(prev_current);

    /* Restore TSS.esp0 to the now-current task's kernel stack */
    process_t *now = process_get_current();
    if (now && now->kstack)
        tss_set_kernel_stack((uint32_t)now->kstack + 8192);

    return rc;
}

/* Inner function: loads ELF into the given process's address space and runs it */
static int elf_exec_argv_inner(const char *path, vfs_node_t *cwd, int argc, char **argv, process_t *proc)
{
    /* ---- Read the file ---- */
    vfs_node_t *file = vfs_resolve(path, cwd);
    if (!file || file->type != VFS_FILE) {
        kprintf("exec: %s: not found\n", path);
        if (proc) { proc->state = PROC_ZOMBIE; proc->exit_code = -1; }
        return -1;
    }

    uint32_t filesz = file->size;
    if (filesz < sizeof(elf32_ehdr_t)) {
        kprintf("exec: %s: too small to be an ELF\n", path);
        if (proc) { proc->state = PROC_ZOMBIE; proc->exit_code = -2; }
        return -2;
    }

    /* Read entire file into a temporary buffer. */
    uint8_t *buf = (uint8_t *)kmalloc(filesz);
    if (!buf) {
        kprintf("exec: out of memory (%u bytes)\n", filesz);
        if (proc) { proc->state = PROC_ZOMBIE; proc->exit_code = -3; }
        return -3;
    }

    uint32_t got = vfs_read(file, 0, filesz, buf);
    if (got < sizeof(elf32_ehdr_t)) {
        kfree(buf);
        kprintf("exec: %s: read error\n", path);
        if (proc) { proc->state = PROC_ZOMBIE; proc->exit_code = -2; }
        return -2;
    }

    /* ---- Validate ELF header ---- */
    elf32_ehdr_t *ehdr = (elf32_ehdr_t *)buf;
    if (!elf_validate(ehdr)) {
        kfree(buf);
        kprintf("exec: %s: not a valid ELF32 i386 executable\n", path);
        if (proc) { proc->state = PROC_ZOMBIE; proc->exit_code = -2; }
        return -2;
    }

    uint32_t entry = ehdr->e_entry;
    if (entry == 0) {
        kfree(buf);
        kprintf("exec: %s: entry point is 0\n", path);
        if (proc) { proc->state = PROC_ZOMBIE; proc->exit_code = -4; }
        return -4;
    }

    serial_printf("[elf] loading %s: entry=%08x phnum=%u (pid=%u pd=%08x)\n",
                  path, entry, ehdr->e_phnum, proc ? proc->pid : 0, proc ? proc->page_dir : 0);

    /* Tracks the highest (vaddr + memsz) across all PT_LOAD segments, so
     * we can seed the process's brk() heap right after the ELF's BSS --
     * exactly where a real libc's initial program break sits. */
    uint32_t max_end = 0;

    /* ---- Load PT_LOAD segments into the process's address space ---- */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        elf32_phdr_t *phdr = (elf32_phdr_t *)(buf + ehdr->e_phoff +
                                               i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD)
            continue;

        uint32_t vaddr  = phdr->p_vaddr;
        uint32_t memsz  = phdr->p_memsz;
        uint32_t filesz2 = phdr->p_filesz;
        uint32_t offset = phdr->p_offset;

        serial_printf("[elf] segment: vaddr=%08x filesz=%u memsz=%u\n",
                      vaddr, filesz2, memsz);

        /* For the first 1 GiB (identity-mapped region), pages are already
         * mapped in both kernel and process page directories (shallow copy).
         * We just need to ensure the data is copied to the virtual address.
         * Since the page tables are shared for this region, a simple memcpy
         * works and is visible in both address spaces. */
        if (vaddr + memsz > 1024 * 1024 * 1024) {
            kfree(buf);
            kprintf("exec: segment at %08x exceeds 1 GiB identity-mapped region\n", vaddr);
            if (proc) { proc->state = PROC_ZOMBIE; proc->exit_code = -3; }
            return -3;
        }
        if (vaddr + memsz > max_end)
            max_end = vaddr + memsz;

        /* Map pages in the process's address space if not already present.
         * For identity-mapped region, they should already be present from
         * vmm_create_address_space(), but we ensure USER flag is set. */
        if (proc && proc->page_dir) {
            for (uint32_t pg = vaddr & ~0xFFF; pg < vaddr + memsz; pg += 0x1000) {
                uint32_t flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
                vmm_map_page_in(proc->page_dir, pg, pg, flags);
            }
        }

        /* Copy file data into memory at vaddr. */
        if (filesz2 > 0 && offset + filesz2 <= got)
            memcpy((void *)vaddr, buf + offset, filesz2);

        /* Zero the BSS portion (memsz > filesz). */
        if (memsz > filesz2)
            memset((void *)(vaddr + filesz2), 0, memsz - filesz2);
    }

    kfree(buf);

    /* Seed brk(): the heap starts right after the last loaded segment
     * (page-aligned up, with one guard page of slack so a slightly-off
     * BSS end never overlaps the first heap page). SYS_BRK grows it from
     * here. See cpu/syscall.c's SYS_BRK handler for the actual mapping. */
    if (proc && max_end > 0) {
        uint32_t heap_start = (max_end + 0x1000) & ~0xFFFu;
        proc->heap_start = heap_start;
        proc->heap_brk   = heap_start;
    }

    /* ---- Set up user stack in the process's address space ---- */
    uint32_t USER_STACK_TOP;
    switch (g_nest) {
        case 1:  USER_STACK_TOP = USER_STACK_TOP_L0; break;
        case 2:  USER_STACK_TOP = USER_STACK_TOP_L1; break;
        case 3:  USER_STACK_TOP = USER_STACK_TOP_L2; break;
        case 4:  USER_STACK_TOP = USER_STACK_TOP_L3; break;
        default: USER_STACK_TOP = USER_STACK_TOP_FALLBACK; break;
    }
    uint32_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;

    /* Ensure stack pages are mapped in process's address space */
    if (proc && proc->page_dir) {
        for (uint32_t pg = stack_base & ~0xFFF; pg < USER_STACK_TOP; pg += 0x1000) {
            uint32_t flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
            vmm_map_page_in(proc->page_dir, pg, pg, flags);
        }
    }

    memset((void *)stack_base, 0, USER_STACK_SIZE);
    uint32_t user_esp = USER_STACK_TOP - 16;   /* leave a little room */
    /* deja 12 bytes con dummy/0/0 por si argv==NULL */
    user_esp -= 12;
    ((uint32_t *)user_esp)[0] = 0;
    ((uint32_t *)user_esp)[1] = 0;
    ((uint32_t *)user_esp)[2] = 0;

    /* ---- Push argv/argc onto the user stack (System-V style) ---- */
    if (argc > 0 && argv) {
        user_esp = USER_STACK_TOP - 16;
        uint32_t str_top = user_esp;
        uint32_t str_ptrs[32];
        if (argc > 32) argc = 32;
        for (int i = argc - 1; i >= 0; i--) {
            int slen = 0; while (argv[i][slen]) slen++;
            slen++;
            str_top -= slen;
            memcpy((void *)str_top, argv[i], slen);
            str_ptrs[i] = str_top;
        }
        str_top &= ~0x3u;
        uint32_t arr = str_top - 4 * (argc + 1);
        for (int i = 0; i < argc; i++)
            ((uint32_t *)arr)[i] = str_ptrs[i];
        ((uint32_t *)arr)[argc] = 0;

        user_esp = arr;
        user_esp -= 12;
        ((uint32_t *)user_esp)[0] = 0;
        ((uint32_t *)user_esp)[1] = (uint32_t)argc;
        ((uint32_t *)user_esp)[2] = arr;
    }

    serial_printf("[elf] jumping to ring 3 at %08x (stack %08x)\n",
                  entry, user_esp);

    /* ---- Drop to ring 3 using save/restore mechanism ---- */
    extern void tss_set_kernel_stack(uint32_t esp0);
    extern int  usermode_save_and_enter(uint32_t entry, uint32_t user_stack,
                                        uint32_t *save_esp);
    extern void elf_arm_exit_jmp(void);
    extern void elf_disarm_exit_jmp(void);
    extern int  elf_get_exit_code(void);

    elf_arm_exit_jmp();

    uint32_t saved_esp = 0;
    if (proc && proc->kstack) {
        tss_set_kernel_stack((uint32_t)proc->kstack + 8192);
    } else {
        uint8_t *kstack = (uint8_t *)kmalloc(4096);
        if (kstack)
            tss_set_kernel_stack((uint32_t)(kstack + 4096));
    }

    (void)usermode_save_and_enter(entry, user_esp, &saved_esp);

    /* ---- Program has exited (via SYS_EXIT longjmp or fault) ---- */
    int exit_code = elf_get_exit_code();
    elf_disarm_exit_jmp();
    if (proc) {
        proc->state     = PROC_ZOMBIE;
        proc->exit_code = exit_code;
    }

    return exit_code;
}

/* ============================================================================
 * elf_execve() — execve() real: reemplaza la imagen del proceso QUE LLAMA
 * (mismo PID) en lugar de crear uno nuevo, tal como el execve(2) de Unix.
 *
 * A diferencia de elf_exec_argv_inner() (usado por `exec`/SYS_SPAWN, que
 * crea un proceso NUEVO y entra a ring 3 de forma anidada vía
 * usermode_save_and_enter + setjmp/longjmp), esta función es llamada
 * DESDE el manejador de SYS_EXECVE mientras se procesa el int 0x80 de un
 * proceso ring-3 ya existente.  No hace ningún salto: solo prepara la
 * nueva imagen y devuelve el (entry, esp) nuevos para que el propio
 * `iret` que ya iba a ejecutar el `int 0x80` salte directo al programa
 * nuevo.  Así, cuando el programa nuevo eventualmente llame SYS_EXIT,
 * usa exactamente el mismo mecanismo (setjmp/longjmp ya armado para
 * este PID desde que fue lanzado originalmente) sin nada especial.
 *
 * Semántica de fallo: si CUALQUIER validación falla, el proceso que
 * llamó NO se toca — sigue corriendo con su imagen original intacta,
 * igual que un execve() real que falla.  Por eso se valida el ELF
 * COMPLETO (incluyendo cada segmento PT_LOAD) antes de sobrescribir un
 * solo byte de la memoria del proceso.
 * ============================================================================ */
int elf_execve(const char *path, vfs_node_t *cwd, int argc, char **argv,
               uint32_t *out_eip, uint32_t *out_esp)
{
    process_t *proc = process_get_current();
    if (!proc) return -1;

    /* ---- Resolver y validar el ELF destino SIN tocar la memoria del
     * proceso que llama.  Real execve(): si falla, el llamador sigue
     * corriendo intacto. ---- */
    vfs_node_t *file = vfs_resolve(path, cwd);
    if (!file || file->type != VFS_FILE) return -1;

    /* SECURITY: a diferencia del exec()/SYS_SPAWN existente (solo
     * alcanzable desde el shell de confianza resolviendo /bin/<cmd>),
     * SYS_EXECVE se invoca directamente desde ring 3, así que SÍ debe
     * respetar el bit de ejecución -- un binario 0700 de otro usuario
     * no debería poder "execve-arse" desde un proceso sin privilegios. */
    if (!vfs_check_access(file, ACC_EXEC)) return -5;

    uint32_t filesz = file->size;
    if (filesz < sizeof(elf32_ehdr_t)) return -2;

    uint8_t *buf = (uint8_t *)kmalloc(filesz);
    if (!buf) return -3;

    uint32_t got = vfs_read(file, 0, filesz, buf);
    if (got < sizeof(elf32_ehdr_t)) { kfree(buf); return -2; }

    elf32_ehdr_t *ehdr = (elf32_ehdr_t *)buf;
    if (!elf_validate(ehdr)) { kfree(buf); return -2; }

    uint32_t entry = ehdr->e_entry;
    if (entry == 0) { kfree(buf); return -4; }

    /* ---- Pase 1 (solo validación): cada segmento PT_LOAD debe caber en
     * el 1 GiB identity-mapped y estar dentro del archivo leído.  Nada
     * se escribe todavía -- esto es lo que hace seguro fallar: si algún
     * segmento es inválido, salimos sin haber tocado un solo byte de la
     * memoria del proceso que llamó. ---- */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        elf32_phdr_t *phdr = (elf32_phdr_t *)(buf + ehdr->e_phoff +
                                               (uint32_t)i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD)
            continue;
        uint64_t end = (uint64_t)phdr->p_vaddr + phdr->p_memsz;
        if (end > 0x40000000ULL) {
            kfree(buf);
            return -3;
        }
        if ((uint64_t)phdr->p_offset + phdr->p_filesz > got) {
            kfree(buf);
            return -2;
        }
    }

    /* ---- argv vive en la memoria del PROPIO llamador, que el Pase 2 de
     * abajo está a punto de sobrescribir -- copiarlo (con sus strings) a
     * kheap primero, igual que ya hace elf_exec_argv() para spawns
     * anidados. ---- */
    char **argv_copy = NULL;
    if (argc > 0 && argv) {
        uint32_t total = 0;
        for (int i = 0; i < argc; i++) {
            int sl = 0; while (argv[i][sl]) sl++;
            total += (uint32_t)sl + 1;
        }
        uint8_t *blob = (uint8_t *)kmalloc(total + (uint32_t)(argc + 1) * sizeof(char *));
        if (!blob) { kfree(buf); return -3; }
        argv_copy = (char **)blob;
        char *str = (char *)(blob + (uint32_t)(argc + 1) * sizeof(char *));
        for (int i = 0; i < argc; i++) {
            argv_copy[i] = str;
            int j = 0;
            while (argv[i][j]) { *str++ = argv[i][j]; j++; }
            *str++ = 0;
        }
        argv_copy[argc] = NULL;
        argv = argv_copy;
    }

    serial_printf("[execve] pid=%u reemplazando imagen con %s: entry=%08x phnum=%u\n",
                  proc->pid, path, entry, ehdr->e_phnum);

    /* ---- Pase 2 (commit): a partir de aquí sobrescribimos la memoria
     * del propio proceso -- ya no hay vuelta atrás, igual que el
     * execve() real empieza a desmapear el address space viejo justo
     * antes de mapear el nuevo. ---- */
    uint32_t max_end = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        elf32_phdr_t *phdr = (elf32_phdr_t *)(buf + ehdr->e_phoff +
                                               (uint32_t)i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD)
            continue;

        uint32_t vaddr   = phdr->p_vaddr;
        uint32_t memsz   = phdr->p_memsz;
        uint32_t filesz2 = phdr->p_filesz;
        uint32_t offset  = phdr->p_offset;

        if (vaddr + memsz > max_end)
            max_end = vaddr + memsz;

        if (proc->page_dir) {
            for (uint32_t pg = vaddr & ~0xFFF; pg < vaddr + memsz; pg += 0x1000) {
                uint32_t flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
                vmm_map_page_in(proc->page_dir, pg, pg, flags);
            }
        }

        if (filesz2 > 0)
            memcpy((void *)vaddr, buf + offset, filesz2);
        if (memsz > filesz2)
            memset((void *)(vaddr + filesz2), 0, memsz - filesz2);
    }

    kfree(buf);

    /* SECURITY/CORRECTNESS: execve() reemplaza por completo el heap del
     * proceso, igual que en Unix real -- cualquier puntero que el
     * programa anterior tuviera hacia SU heap deja de tener sentido con
     * el ELF nuevo mapeado encima. Sin este reset, un proceso podría
     * heredar el heap_brk del programa viejo (potencialmente MÁS ALTO
     * que el final del BSS del programa nuevo) y SYS_BRK aceptaría
     * `new_brk` menores a ese valor heredado como "encoger", dejando
     * mapeadas páginas de heap del programa anterior visibles para el
     * nuevo -- una fuga de datos entre programas ejecutados vía execve()
     * en el mismo proceso. */
    if (max_end > 0) {
        uint32_t heap_start = (max_end + 0x1000) & ~0xFFFu;
        proc->heap_start = heap_start;
        proc->heap_brk   = heap_start;
    }

    /* ---- Stack de usuario nuevo, reusando el MISMO nivel de anidamiento
     * en el que ya corría el proceso llamador (ver USER_STACK_TOP_L* más
     * arriba): execve() reemplaza el programa en su lugar, no añade un
     * nivel nuevo de anidamiento. ---- */
    uint32_t USER_STACK_TOP;
    switch (g_nest) {
        case 1:  USER_STACK_TOP = USER_STACK_TOP_L0; break;
        case 2:  USER_STACK_TOP = USER_STACK_TOP_L1; break;
        case 3:  USER_STACK_TOP = USER_STACK_TOP_L2; break;
        case 4:  USER_STACK_TOP = USER_STACK_TOP_L3; break;
        default: USER_STACK_TOP = USER_STACK_TOP_FALLBACK; break;
    }
    uint32_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;

    if (proc->page_dir) {
        for (uint32_t pg = stack_base & ~0xFFF; pg < USER_STACK_TOP; pg += 0x1000) {
            uint32_t flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
            vmm_map_page_in(proc->page_dir, pg, pg, flags);
        }
    }
    memset((void *)stack_base, 0, USER_STACK_SIZE);

    uint32_t user_esp = USER_STACK_TOP - 16;
    user_esp -= 12;
    ((uint32_t *)user_esp)[0] = 0;
    ((uint32_t *)user_esp)[1] = 0;
    ((uint32_t *)user_esp)[2] = 0;

    if (argc > 0 && argv) {
        user_esp = USER_STACK_TOP - 16;
        uint32_t str_top = user_esp;
        uint32_t str_ptrs[32];
        if (argc > 32) argc = 32;
        for (int i = argc - 1; i >= 0; i--) {
            int slen = 0; while (argv[i][slen]) slen++;
            slen++;
            str_top -= (uint32_t)slen;
            memcpy((void *)str_top, argv[i], (uint32_t)slen);
            str_ptrs[i] = str_top;
        }
        str_top &= ~0x3u;
        uint32_t arr = str_top - 4 * (uint32_t)(argc + 1);
        for (int i = 0; i < argc; i++)
            ((uint32_t *)arr)[i] = str_ptrs[i];
        ((uint32_t *)arr)[argc] = 0;

        user_esp = arr;
        user_esp -= 12;
        ((uint32_t *)user_esp)[0] = 0;
        ((uint32_t *)user_esp)[1] = (uint32_t)argc;
        ((uint32_t *)user_esp)[2] = arr;
    }

    if (argv_copy) kfree(argv_copy);

    /* Renombrar el proceso para reflejar el programa nuevo -- mismo PID,
     * mismo padre, mismos fds/cwd, exactamente como un execve() real. */
    const char *slash = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p + 1;
    strncpy(proc->name, slash, PROC_NAME_MAX - 1);
    proc->name[PROC_NAME_MAX - 1] = '\0';

    serial_printf("[execve] pid=%u listo: eip=%08x esp=%08x\n",
                  proc->pid, entry, user_esp);

    *out_eip = entry;
    *out_esp = user_esp;
    return 0;
}
