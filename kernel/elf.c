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
 *     we also support lower addresses within the identity-mapped 256 MiB).
 *   - A user stack is allocated at a fixed high address within the
 *     identity-mapped region.
 *   - Everything runs in the kernel's single address space (identity-mapped),
 *     with ring 3 privilege enforced by the GDT selectors.
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

/* Where we allocate the user stack (within identity-mapped 256 MiB). */
/* Cada nivel de anidamiento usa un user stack en VA distinta para no
 * pisar el stack del padre. Aún sin address space por proceso real, esto
 * basta para shell→hijo→nieto (3 niveles). Cada nivel reserva 64 KB.
 *
 * Nivel 0 (shell)    : 0x0F000000  (256 KiB hacia abajo)
 * Nivel 1 (cmd)      : 0x0EE00000
 * Nivel 2 (sub-cmd)  : 0x0EC00000
 *
 * Las VAs están dentro del identity-map de 256 MiB, así que el CPU las ve
 * en cualquier momento. La protección de aislamiento depende del bit User
 * de las páginas (ya activado en vmm_init para el identity map). */
#define USER_STACK_SIZE     0x10000   /* 64 KiB por nivel */
#define USER_STACK_TOP_L0   0x0F000000U
#define USER_STACK_TOP_L1   0x0EE00000U
#define USER_STACK_TOP_L2   0x0EC00000U
#define USER_STACK_TOP_L3   0x0EA00000U   /* por si acaso */
#define USER_STACK_TOP_FALLBACK 0x0E800000U

/* Rebase del segundo nivel de recursión (cuando un ELF ring-3 hace
 * SPAWN de otro ELF): cargamos al hijo a OFFSET arriba de donde linkeó,
 * para no pisar el código del padre que vive en 0x08048000.
 *
 * Es una "reubicación a mano" pre-padding del p_vaddr: cada PT_LOAD se
 * carga en (p_vaddr + g_elf_rebase) y el entry point se ajusta.
 *
 * Limitación: SOLO funciona si el ELF es position-independent O si el
 * tamaño total cabe sin que se referencien direcciones absolutas.
 * Nuestros coreutils están enlazados a 0x08048000 fijo, así que un
 * rebase rompería sus accesos absolutos.
 *
 * Alternativa más simple y correcta: GUARDAR Y RESTAURAR la región
 * 0x08048000..0x08100000 (768 KB) alrededor del spawn anidado.
 * Eso es lo que hacemos abajo, con un kmalloc temporal. */
static uint8_t *g_padre_backup;
static uint32_t g_padre_backup_size;
#define PADRE_SAVE_BASE  0x08048000
#define PADRE_SAVE_SIZE  0x000B8000   /* 736 KB — cubre code+data+bss del shell */

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
 * saber cuándo respaldar el código del padre. */
static int g_nest = 0;
static int elf_exec_argv_inner(const char *path, vfs_node_t *cwd, int argc, char **argv);

int elf_exec_argv(const char *path, vfs_node_t *cwd, int argc, char **argv)
{
    /* Si vamos a entrar a un nivel >=1, guardamos la región de código
     * del padre porque el hijo se cargará en la misma VA. */
    uint8_t *backup = NULL;
    char   **argv_copy = NULL;
    int      did_save = 0;
    if (g_nest >= 1) {
        backup = (uint8_t *)kmalloc(PADRE_SAVE_SIZE);
        if (backup) {
            memcpy(backup, (void *)PADRE_SAVE_BASE, PADRE_SAVE_SIZE);
            did_save = 1;
        }
        /* También argv vive en el espacio del padre (su stack/scratch).
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

    int rc = elf_exec_argv_inner(path, cwd, argc, argv);

    g_nest--;
    if (did_save && backup) {
        memcpy((void *)PADRE_SAVE_BASE, backup, PADRE_SAVE_SIZE);
        kfree(backup);
    }
    if (argv_copy) kfree(argv_copy);
    return rc;
}

static int elf_exec_argv_inner(const char *path, vfs_node_t *cwd, int argc, char **argv)
{
    /* ---- Read the file ---- */
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

    /* Read entire file into a temporary buffer. */
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

    /* ---- Validate ELF header ---- */
    elf32_ehdr_t *ehdr = (elf32_ehdr_t *)buf;
    if (!elf_validate(ehdr)) {
        kfree(buf);
        kprintf("exec: %s: not a valid ELF32 i386 executable\n", path);
        return -2;
    }

    uint32_t entry = ehdr->e_entry;
    if (entry == 0) {
        kfree(buf);
        kprintf("exec: %s: entry point is 0\n", path);
        return -4;
    }

    serial_printf("[elf] loading %s: entry=%08x phnum=%u\n",
                  path, entry, ehdr->e_phnum);

    /* ---- Load PT_LOAD segments ---- */
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

        /* Ensure the pages are mapped.  For addresses within the
         * identity-mapped 256 MiB, they already are.  For higher
         * addresses, we'd need to allocate pages — but standard i386
         * ELFs link at 0x08048000 which is within 256 MiB. */
        if (vaddr + memsz > 256 * 1024 * 1024) {
            kfree(buf);
            kprintf("exec: segment at %08x exceeds identity-mapped region\n",
                    vaddr);
            return -3;
        }

        /* Copy file data into memory at vaddr. */
        if (filesz2 > 0 && offset + filesz2 <= got)
            memcpy((void *)vaddr, buf + offset, filesz2);

        /* Zero the BSS portion (memsz > filesz). */
        if (memsz > filesz2)
            memset((void *)(vaddr + filesz2), 0, memsz - filesz2);
    }

    kfree(buf);

    /* ---- Set up user stack ----
     * Cada nivel de anidamiento usa su propia VA para no pisar el padre.
     * `g_nest` ya fue incrementado por el wrapper antes de llamar aquí.
     */
    uint32_t USER_STACK_TOP;
    switch (g_nest) {
        case 1:  USER_STACK_TOP = USER_STACK_TOP_L0; break;
        case 2:  USER_STACK_TOP = USER_STACK_TOP_L1; break;
        case 3:  USER_STACK_TOP = USER_STACK_TOP_L2; break;
        case 4:  USER_STACK_TOP = USER_STACK_TOP_L3; break;
        default: USER_STACK_TOP = USER_STACK_TOP_FALLBACK; break;
    }
    uint32_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    memset((void *)stack_base, 0, USER_STACK_SIZE);
    uint32_t user_esp = USER_STACK_TOP - 16;   /* leave a little room */
    /* deja 12 bytes con dummy/0/0 por si argv==NULL */
    user_esp -= 12;
    ((uint32_t *)user_esp)[0] = 0;
    ((uint32_t *)user_esp)[1] = 0;
    ((uint32_t *)user_esp)[2] = 0;

    /* ---- Push argv/argc onto the user stack (System-V style) ---- */
    /* Layout (high -> low):
     *   [strings...]   argv[i] string data
     *   [argv_ptrs]    char *argv[argc+1]   (last = NULL)
     *   argv_ptr       char **argv
     *   argc           int
     *   ret_dummy      (no se usa, el _start hace exit)
     */
    if (argc > 0 && argv) {
        /* recalculamos: usamos el USER_STACK_TOP del nivel actual */
        user_esp = USER_STACK_TOP - 16;
        /* primero copiamos las cadenas */
        uint32_t str_top = user_esp;
        uint32_t str_ptrs[32];
        if (argc > 32) argc = 32;
        for (int i = argc - 1; i >= 0; i--) {
            int slen = 0; while (argv[i][slen]) slen++;
            slen++;  /* NUL */
            str_top -= slen;
            memcpy((void *)str_top, argv[i], slen);
            str_ptrs[i] = str_top;
        }
        /* alineamos a 4 */
        str_top &= ~0x3u;

        /* array de punteros + NUL */
        uint32_t arr = str_top - 4 * (argc + 1);
        for (int i = 0; i < argc; i++)
            ((uint32_t *)arr)[i] = str_ptrs[i];
        ((uint32_t *)arr)[argc] = 0;

        /* argc, argv en el tope del stack — pero NO los pusheamos como
         * args de call (el crt0 lo hace).  Sólo dejamos AAA libre y
         * exponemos esp para que pueda construir su frame. */
        user_esp = arr;
        /* dejamos en [esp] = 0 dummy, [esp+4] = argc, [esp+8] = argv */
        user_esp -= 12;
        ((uint32_t *)user_esp)[0] = 0;            /* return addr dummy */
        ((uint32_t *)user_esp)[1] = (uint32_t)argc;
        ((uint32_t *)user_esp)[2] = arr;          /* argv */
    }

    /* ---- Register as a process ---- */
    char name[32];
    /* Extract just the filename from the path. */
    const char *slash = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p + 1;
    strncpy(name, slash, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    /* Create the process entry BEFORE swapping `current`, so the new task
     * exists in the run queue while it runs.  Mark it RUNNING and stash
     * the shell's current so we can restore it when the program returns
     * (or exits via SYS_EXIT — see process_exit() handling below). */
    process_t *proc = process_create(name, NULL);
    process_t *prev_current = process_get_current();
    if (proc) {
        proc->state = PROC_RUNNING;
        /* Make this proc the current task while it runs.  This is what
         * makes `int 0x80 / SYS_EXIT` mark the RIGHT task as a zombie
         * (previously SYS_EXIT killed the shell because `current` still
         * pointed at it — the user-visible symptom was that `top` kept
         * showing terminated ELFs as "RUN" forever). */
        process_set_current(proc);
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
    extern uint32_t elf_get_saved_esp_ptr(void);

    /* Arm the SYS_EXIT longjmp landing pad: when the program calls
     * SYS_EXIT (or page-faults), control unwinds here right after the
     * usermode_save_and_enter() call below — without ever executing the
     * bogus `ret` that the user-stack would otherwise dispatch to. */
    elf_arm_exit_jmp();

    /* Update the TSS so the next ring3 -> ring0 trap (int 0x80, IRQs,
     * exceptions) lands on a valid kernel stack. We use the new task's
     * kstack if it has one, otherwise we reuse a chunk of our own
     * current kernel stack: usermode_save_and_enter saves ESP into
     * `saved_esp` before doing iret, so the trap stack is fine as long
     * as we point esp0 above that saved frame. */
    uint32_t saved_esp = 0;
    if (proc && proc->kstack) {
        tss_set_kernel_stack((uint32_t)proc->kstack + 8192);
    } else {
        /* Allocate a small kernel stack just for ring3 traps. */
        uint8_t *kstack = (uint8_t *)kmalloc(4096);
        if (kstack)
            tss_set_kernel_stack((uint32_t)(kstack + 4096));
    }

    /* Make sure interrupts will be enabled in user mode (the iret frame
     * built by usermode_save_and_enter already sets IF in EFLAGS). */

    /* JUMP TO RING 3.  This call returns:
     *   - normally with the value passed to usermode_exit_resume() when
     *     SYS_EXIT (or a fault-killed program) longjmps back via the
     *     exit pad armed above, OR
     *   - via the longjmp itself, in which case we never touch this
     *     return value — we get back into elf_exec() right after
     *     elf_arm_exit_jmp() with a non-zero setjmp return and skip
     *     past this call entirely.
     * Either way, when control gets to the next line, the user program
     * is done. */
    (void)usermode_save_and_enter(entry, user_esp, &saved_esp);

    /* ---- Restore cursor position so the shell prompt appears correctly ---- */
    /* Don't call vga_init() here — it clears the screen and erases the
     * program's output.  Programs that use print() / print_num() go through
     * SYS_WRITE → vga_putchar(), which already maintains correct VGA state.
     * Only programs that write directly to 0xB8000 (vga_clear, vga_putchar
     * builtins) might leave the cursor in a weird spot, but even then the
     * shell's print_prompt() will fix it on the next iteration. */

    /* If we got here either:
     *   (a) the program ran to completion and returned from fn() (rare
     *       — tcc's stub always does SYS_EXIT before any real ret), or
     *   (b) SYS_EXIT / a fatal exception longjmp'd back to elf_exec
     *       through the elf_arm_exit_jmp() pad — which is the common
     *       path.
     * Either way, mark the proc zombie, drop the exit jmp so a later
     * unrelated fault doesn't accidentally land here, and restore the
     * shell as `current` so its prompt comes back. */
    int exit_code = elf_get_exit_code();
    elf_disarm_exit_jmp();
    if (proc) {
        proc->state     = PROC_ZOMBIE;
        proc->exit_code = exit_code;
    }
    if (prev_current) process_set_current(prev_current);

    /* Restore TSS.esp0 to the shell's (or whatever now-current task)
     * kernel stack, so any later ring3 trap from a follow-up program
     * starts from a clean slate. */
    process_t *now = process_get_current();
    if (now && now->kstack)
        tss_set_kernel_stack((uint32_t)now->kstack + 8192);

    return exit_code;
}
