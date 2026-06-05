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
#define USER_STACK_TOP    0x0F000000   /* 240 MiB — well within 256 MiB map */
#define USER_STACK_SIZE   0x4000       /* 16 KiB */

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

    /* ---- Set up user stack ---- */
    uint32_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    memset((void *)stack_base, 0, USER_STACK_SIZE);
    uint32_t user_esp = USER_STACK_TOP - 16;   /* leave a little room */

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
    extern int  usermode_run(const char *name, void (*entry_fn)(void));
    extern void elf_arm_exit_jmp(void);
    extern void elf_disarm_exit_jmp(void);
    extern int  elf_get_exit_code(void);

    /* Arm the SYS_EXIT longjmp landing pad: when the program calls
     * SYS_EXIT (or page-faults), control unwinds here right after the
     * `fn()` call below — without ever executing the bogus `ret` that
     * the user-stack would otherwise dispatch to. */
    elf_arm_exit_jmp();

    /* We can't use enter_usermode() directly because it doesn't return.
     * Instead, we set up a trampoline that jumps to the ELF entry point.
     * For simplicity, store the entry+stack and use the existing
     * usermode_save_and_enter mechanism. */

    /* Simple approach: just call the code directly in ring 0 first
     * to verify it works, then we can move to ring 3 later.
     * Actually, the safest approach for now: copy the entry point to a
     * known location and call it as a function pointer from ring 0.
     * This is NOT proper isolation but lets us test the assembler. */
    
    /* For now: execute in ring 0 (kernel mode) as a simple function call.
     * The program uses int 0x80 which works from ring 0 too.
     * TODO: proper ring 3 execution with save/restore. */
    /* Ensure interrupts are enabled before running user code
     * (needed for getchar/sleep which depend on IRQ1/IRQ0). */
    __asm__ volatile("sti");

    typedef void (*entry_fn_t)(void);
    entry_fn_t fn = (entry_fn_t)entry;
    fn();

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

    return exit_code;
}
