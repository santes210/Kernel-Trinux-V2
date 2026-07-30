/* kernel/elf.c  -  ELF32 executable loader.
 *
 * Reads an ELF32 executable from the VFS, loads its PT_LOAD segments into
 * FRAMES FRESCOS del PMM dentro del address space PRIVADO del proceso
 * (v0.6.0), and jumps to the entry point in ring 3 (userspace) con el
 * CR3 del propio proceso.
 *
 * The user program communicates with the kernel ONLY via int 0x80 syscalls.
 * When it calls SYS_EXIT, control returns here (setjmp/longjmp con dueño).
 *
 * Memory layout for user programs:
 *   - Cada proceso vive en su PROPIO address space: su page directory
 *     tiene page tables PRIVADAS para la región de usuario
 *     0x08000000-0x10000000 (tablas 32-63).  Kernel, physmap y MMIO se
 *     comparten con el page directory del kernel (vmm.c).
 *   - El código/datos del ELF se cargan en frames PMM nuevos; el stack
 *     de usuario vive siempre en la misma VA (0x0F000000) pero en
 *     frames físicos distintos por proceso.  Anidar spawns ya NO
 *     requiere backups de 736 KiB ni stacks por nivel de anidamiento.
 */
#include "elf.h"
#include "../fs/vfs.h"
#include "../mm/kheap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../cpu/syscall.h"
#include "../process/process.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/serial.h"

/* FIX (v0.5.3): declaración a nivel de archivo (antes se declaraba dentro
 * de una función DESPUÉS de usarse -> implicit-declaration warning). */
extern void tss_set_kernel_stack(uint32_t esp0);

/* Dirección mínima aceptable para un PT_LOAD de un ELF de usuario. Todo
 * lo inferior es espacio del kernel: un ELF con PT_LOAD en 0x00100000
 * haría que el loader sobrescribiera el propio kernel. */
#define ELF_MIN_USER_VADDR  0x08000000U

/* Y máxima: la región privada de usuario termina en 0x10000000
 * (USER_SPACE_END). Antes se aceptaba hasta 1 GiB, pero con las tablas
 * 64-255 cerradas a ring 3 (physmap supervisor) un segmento ahí ya no
 * es ejecutable: límites coherentes con uaccess. */
#define ELF_MAX_USER_VADDR  0x10000000U

/* User stack: una sola cima canónica (64 KiB hacia abajo) en las tablas
 * PRIVADAS de cada proceso. No existen niveles L0..L3 ni backups del
 * padre: el aislamiento por proceso los hizo innecesarios. */
#define USER_STACK_SIZE     0x10000u          /* 64 KiB */
#define USER_STACK_TOP      0x0F000000u

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

/* ============================================================================
 * Carga de segmentos en FRAMES FRESCHOS (v0.6.0)
 *
 * Mapea en las tablas PRIVADAS de `proc` las páginas [vaddr, vaddr+memsz)
 * con un frame PMM nuevo por página (ceros), copiando la parte file-backed
 * dentro de cada frame. El frame se toca por su VA identidad (>= 256 MiB,
 * physmap supervisor escribible en ring 0 con cualquier CR3 activo).
 * ==========================================================================*/
static int map_segment_pages(process_t *proc,
                             uint32_t vaddr,
                             const uint8_t *file_data, uint32_t filesz,
                             uint32_t memsz)
{
    for (uint32_t pg = vaddr & ~0xFFFu; pg < vaddr + memsz; pg += 0x1000) {
        uint32_t fr = pmm_alloc_frame();
        if (!fr) return -3;   /* sin memoria física */
        memset((void *)fr, 0, 4096);

        /* Tile de solapamiento página vs parte file-backed del segmento */
        uint32_t lo = (pg > vaddr) ? pg : vaddr;
        uint32_t hi = vaddr + filesz;
        if (hi > pg + 0x1000u) hi = pg + 0x1000u;
        if (file_data && lo < hi)
            memcpy((void *)(fr + (lo - pg)), file_data + (lo - vaddr), hi - lo);

        vmm_map_page_in(proc->page_dir, pg, fr,
                        PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }
    return 0;
}

/* Mapea el stack de usuario canónico (frame fresco por página, ceros). */
static int map_user_stack(process_t *proc)
{
    return map_segment_pages(proc, USER_STACK_TOP - USER_STACK_SIZE,
                             NULL, 0, USER_STACK_SIZE);
}

/* ============================================================================
 * Construcción de argc/argv/en la cima del user stack del proceso.
 *
 * Misma disposición System-V que siempre: strings descendentes desde
 * USER_STACK_TOP-16, punteros, y [esp]=0,[esp+4]=argc,[esp+8]=argv.
 * Se construye en un staging de kheap y se copia con vmm_copy_to_user(),
 * porque las VAs destino viven en las tablas PRIVADAS del proceso.
 * ==========================================================================*/
static int build_user_stack(process_t *proc, int argc, char **argv,
                            uint32_t *out_esp)
{
    *out_esp = 0;

    if (argc <= 0 || !argv) {
        /* Stack vacío con 3 dwords dummy (compat con _start legacy). */
        uint32_t zeros[3] = {0, 0, 0};
        uint32_t esp = USER_STACK_TOP - 16 - 12;
        if (vmm_copy_to_user(proc->page_dir, esp, zeros, sizeof(zeros)))
            return -3;
        *out_esp = esp;
        return 0;
    }

    if (argc > 32) argc = 32;

    uint32_t total = 0;
    for (int i = 0; i < argc; i++) {
        int sl = 0; while (argv[i][sl]) sl++;
        total += (uint32_t)sl + 1;
    }
    if (total + 4u * (argc + 1) + 32 > USER_STACK_SIZE / 2)
        return -3;   /* argv monstruoso: no construir un stack inseguro */

    /* Calcular layout sobre VAs absolutas (como siempre). */
    uint32_t str_top  = USER_STACK_TOP - 16;
    uint32_t first_va = str_top;       /* VA más baja que tocaremos */
    /* strings (descendente) */
    for (int i = argc - 1; i >= 0; i--) {
        int sl = 0; while (argv[i][sl]) sl++;
        str_top -= (uint32_t)sl + 1;
        if (str_top < first_va) first_va = str_top;
    }
    uint32_t arr = (str_top & ~0x3u) - 4u * (uint32_t)(argc + 1);
    uint32_t esp = arr - 12;
    if (esp < first_va) first_va = esp;

    /* Staging: imagen del intervalo [esp, USER_STACK_TOP -16). Nota: el
     * hueco [USER_STACK_TOP-16, USER_STACK_TOP) no se toca (padding). */
    uint32_t span = (USER_STACK_TOP - 16) - esp;
    uint8_t *stg = (uint8_t *)kmalloc(span);
    if (!stg) return -3;
    memset(stg, 0, span);

    uint32_t cur = USER_STACK_TOP - 16;
    uint32_t str_ptrs[32];
    for (int i = argc - 1; i >= 0; i--) {
        int sl = 0; while (argv[i][sl]) sl++;
        cur -= (uint32_t)sl + 1;
        memcpy(stg + (cur - esp), argv[i], (uint32_t)sl + 1);
        str_ptrs[i] = cur;
    }
    for (int i = 0; i < argc; i++)
        ((uint32_t *)(stg + (arr - esp)))[i] = str_ptrs[i];
    ((uint32_t *)(stg + (arr - esp)))[argc] = 0;

    /* [esp]=0, [esp+4]=argc, [esp+8]=arr */
    ((uint32_t *)stg)[1] = (uint32_t)argc;
    ((uint32_t *)stg)[2] = arr;

    int rc = vmm_copy_to_user(proc->page_dir, esp, stg, span);
    kfree(stg);
    if (rc) return -3;
    *out_esp = esp;
    return 0;
}

/* ============================================================================
 * Lectura + validación completa del ELF (sin tocar memoria del proceso).
 * Devuelve 0 ok y deja entry/max_end; o código de error negativo:
 *   -1 not found, -2 no-ELF/bounds, -3 oom/rango, -4 entry 0
 * ==========================================================================*/
static int read_elf(const char *path, vfs_node_t *cwd,
                    uint8_t **out_buf, uint32_t *out_got)
{
    vfs_node_t *file = vfs_resolve(path, cwd);
    if (!file || file->type != VFS_FILE) {
        kprintf("exec: %s: not found\n", path);
        return -1;
    }
    uint32_t filesz = file->size;
    if (filesz < sizeof(elf32_ehdr_t)) {
        kprintf("exec: %s: too small to be an ELF\n", path);
        return -2;
    }
    uint8_t *buf = (uint8_t *)kmalloc(filesz);
    if (!buf) {
        kprintf("exec: out of memory (%u bytes)\n", filesz);
        return -3;
    }
    uint32_t got = vfs_read(file, 0, filesz, buf);
    if (got < sizeof(elf32_ehdr_t)) {
        kfree(buf);
        kprintf("exec: %s: read error\n", path);
        return -2;
    }
    *out_buf = buf;
    *out_got = got;
    return 0;
}

static int validate_phdrs(const uint8_t *buf, uint32_t got,
                          const elf32_ehdr_t *ehdr)
{
    /* SECURITY FIX (v0.5.3): la tabla de program headers debe caber en
     * el archivo leído (antes: OOB read con e_phnum/e_phoff corruptos). */
    if (ehdr->e_phentsize < sizeof(elf32_phdr_t) ||
        (uint64_t)ehdr->e_phoff +
        (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > got)
        return -2;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *phdr = (const elf32_phdr_t *)(buf + ehdr->e_phoff +
                                                (uint32_t)i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD)
            continue;
        uint64_t end = (uint64_t)phdr->p_vaddr + phdr->p_memsz;
        if (end > ELF_MAX_USER_VADDR)
            return -3;
        if (phdr->p_vaddr < ELF_MIN_USER_VADDR)
            return -3;
        if ((uint64_t)phdr->p_offset + phdr->p_filesz > got)
            return -2;
    }
    return 0;
}

/* Copia alta/baja entre dos VAs: end del último segmento para brk(). */
static void track_max_end(const elf32_ehdr_t *ehdr, const uint8_t *buf,
                          uint32_t *max_end)
{
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *phdr = (const elf32_phdr_t *)(buf + ehdr->e_phoff +
                                                (uint32_t)i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_vaddr + phdr->p_memsz > *max_end)
            *max_end = phdr->p_vaddr + phdr->p_memsz;
    }
}

static int load_segments(process_t *proc, const uint8_t *buf,
                         const elf32_ehdr_t *ehdr)
{
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *phdr = (const elf32_phdr_t *)(buf + ehdr->e_phoff +
                                                (uint32_t)i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD)
            continue;
        serial_printf("[elf] segment: vaddr=%08x filesz=%u memsz=%u\n",
                      phdr->p_vaddr, phdr->p_filesz, phdr->p_memsz);
        int rc = map_segment_pages(proc, phdr->p_vaddr,
                                   buf + phdr->p_offset,
                                   phdr->p_filesz, phdr->p_memsz);
        if (rc) return rc;
    }
    return 0;
}

/* Seed del brk() justo después del último segmento (alineado a página). */
static void seed_brk(process_t *proc, uint32_t max_end)
{
    if (proc && max_end > 0) {
        uint32_t heap_start = (max_end + 0x1000u) & ~0xFFFu;
        proc->heap_start = heap_start;
        proc->heap_brk   = heap_start;
    }
}

static void proc_dies(process_t *proc, int code)
{
    if (proc) { proc->state = PROC_ZOMBIE; proc->exit_code = code; }
}

int elf_exec(const char *path, vfs_node_t *cwd)
{
    return elf_exec_argv(path, cwd, 0, NULL);
}

static int elf_exec_argv_inner(const char *path, vfs_node_t *cwd,
                               int argc, char **argv, process_t *proc);

/* Wrapper: crea el proceso y lo ejecuta SIN backups ni niveles de
 * anidamiento (v0.6.0 — el aislamiento por address space los reemplaza). */
int elf_exec_argv(const char *path, vfs_node_t *cwd, int argc, char **argv)
{
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

    /* Restore previous current + TSS del flujo que sigue. */
    if (prev_current) process_set_current(prev_current);
    process_t *now = process_get_current();
    if (now && now->kstack)
        tss_set_kernel_stack((uint32_t)now->kstack + 8192);

    return rc;
}

/* Inner function: loads the ELF into the process's PRIVATE address space
 * and runs it in ring 3 with the process's CR3. */
static int elf_exec_argv_inner(const char *path, vfs_node_t *cwd,
                               int argc, char **argv, process_t *proc)
{
    uint8_t *buf = NULL;
    uint32_t got = 0;
    int rc = read_elf(path, cwd, &buf, &got);
    if (rc) { proc_dies(proc, rc); return rc; }

    elf32_ehdr_t *ehdr = (elf32_ehdr_t *)buf;
    if (!elf_validate(ehdr)) {
        kfree(buf);
        kprintf("exec: %s: not a valid ELF32 i386 executable\n", path);
        proc_dies(proc, -2);
        return -2;
    }

    uint32_t entry = ehdr->e_entry;
    if (entry == 0) {
        kfree(buf);
        kprintf("exec: %s: entry point is 0\n", path);
        proc_dies(proc, -4);
        return -4;
    }

    rc = validate_phdrs(buf, got, ehdr);
    if (rc) {
        kfree(buf);
        kprintf("exec: %s: invalid program headers (rc=%d)\n", path, rc);
        proc_dies(proc, rc);
        return rc;
    }

    serial_printf("[elf] loading %s: entry=%08x phnum=%u (pid=%u pd=%08x)\n",
                  path, entry, ehdr->e_phnum, proc ? proc->pid : 0,
                  proc ? proc->page_dir : 0);

    /* ---- Cargar la imagen en frames frescos del address space privado ---- */
    uint32_t max_end = 0;
    track_max_end(ehdr, buf, &max_end);

    rc = load_segments(proc, buf, ehdr);
    if (rc) {
        kfree(buf);
        kprintf("exec: %s: out of physical memory\n", path);
        proc_dies(proc, rc);
        return rc;
    }
    kfree(buf);

    seed_brk(proc, max_end);

    /* ---- User stack (frames frescos) + argv ---- */
    rc = map_user_stack(proc);
    if (rc) {
        kprintf("exec: %s: out of memory for user stack\n", path);
        proc_dies(proc, rc);
        return rc;
    }

    uint32_t user_esp = 0;
    rc = build_user_stack(proc, argc, argv, &user_esp);
    if (rc) {
        kprintf("exec: %s: argv too big\n", path);
        proc_dies(proc, rc);
        return rc;
    }

    serial_printf("[elf] jumping to ring 3 at %08x (stack %08x)\n",
                  entry, user_esp);

    /* ---- Entrar a ring 3 con el CR3 del proceso (v0.6.0) ---- */
    extern int      usermode_save_and_enter(uint32_t entry, uint32_t user_stack,
                                            uint32_t *save_esp);
    extern int      elf_arm_exit_jmp(void);     /* 0 primera pasada */
    extern void     elf_disarm_exit_jmp(void);
    extern bool     elf_jmp_still_armed(void);
    extern uint32_t elf_jmp_saved_cr3(void);
    extern int      elf_get_exit_code(void);

    int arm_rc = elf_arm_exit_jmp();
    if (arm_rc < 0) {           /* sin slots: demasiados niveles */
        proc_dies(proc, -5);
        return -5;
    }

    uint32_t saved_esp = 0;
    if (arm_rc == 0 && elf_jmp_still_armed()) {
        /* PRIMERA PASADA (guard redundante contra el retorno del longjmp) */
        if (proc->kstack)
            tss_set_kernel_stack((uint32_t)proc->kstack + 8192);
        vmm_switch_address_space(proc->page_dir);
        (void)usermode_save_and_enter(entry, user_esp, &saved_esp);
    }

    /* ---- El programa salió (SYS_EXIT/fault → longjmp al arm) ---- */
    vmm_switch_address_space(elf_jmp_saved_cr3());
    int exit_code = elf_get_exit_code();
    elf_disarm_exit_jmp();
    proc_dies(proc, exit_code);

    return exit_code;
}

/* ============================================================================
 * elf_execve() — execve() real: reemplaza la imagen del proceso QUE LLAMA
 * (mismo PID). Llamada DESDE el manejador de SYS_EXECVE con el int 0x80 del
 * proceso en curso: no salta nada; prepara la imagen nueva y devuelve
 * (entry, esp) para que el `iret` que ya iba a ejecutar el int 0x80 salte
 * directo al programa nuevo. La salida posterior (SYS_EXIT) usa el mismo
 * jump buffer armado para este PID desde su lanzamiento original — el
 * owner del buffer es el pid, y execve() NO cambia el pid. ✓
 *
 * Semántica de fallo: si CUALQUIER validación falla, el proceso llamador
 * sigue corriendo con su imagen intacta (execve(2) real).
 * ==========================================================================*/
int elf_execve(const char *path, vfs_node_t *cwd, int argc, char **argv,
               uint32_t *out_eip, uint32_t *out_esp)
{
    process_t *proc = process_get_current();
    if (!proc) return -1;

    vfs_node_t *file = vfs_resolve(path, cwd);
    if (!file || file->type != VFS_FILE) return -1;

    /* SECURITY: SYS_EXECVE se invoca desde ring 3, así que SÍ respeta el
     * bit de ejecución (un binario 0700 ajeno no debe ser ejecutable). */
    if (!vfs_check_access(file, ACC_EXEC)) return -5;

    uint8_t *buf = NULL;
    uint32_t got = 0;
    int rc = read_elf(path, cwd, &buf, &got);
    if (rc) return rc;

    elf32_ehdr_t *ehdr = (elf32_ehdr_t *)buf;
    if (!elf_validate(ehdr)) { kfree(buf); return -2; }

    uint32_t entry = ehdr->e_entry;
    if (entry == 0) { kfree(buf); return -4; }

    rc = validate_phdrs(buf, got, ehdr);
    if (rc) { kfree(buf); return rc; }

    /* argv vive en la memoria del PROPIO llamador, que vmm_reset_user_region
     * está a punto de liberar — copiarlo (con strings) a kheap primero. */
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

    /* ---- Commit: libera la imagen vieja y carga la nueva ---- */
    uint32_t max_end = 0;
    track_max_end(ehdr, buf, &max_end);

    if (proc->page_dir)
        vmm_reset_user_region(proc->page_dir);   /* libera frames viejos */

    /* NOTA: a partir de aquí el código/datos/stack viejos están
     * desmapeados. Seguimos a salvo en ring 0 (kstack + kernel tables
     * compartidas) y antes del iret todo lo nuevo queda mapeado. */

    rc = load_segments(proc, buf, ehdr);
    if (rc == 0) rc = map_user_stack(proc);
    uint32_t user_esp = 0;
    if (rc == 0) rc = build_user_stack(proc, argc, argv, &user_esp);
    kfree(buf);
    if (argv_copy) kfree(argv_copy);
    if (rc) {
        /* No hay vuelta atrás: terminar el proceso (exit code de error). */
        proc_dies(proc, rc);
        return rc;
    }

    seed_brk(proc, max_end);

    /* Renombrar el proceso (mismo PID, como execve real). */
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
