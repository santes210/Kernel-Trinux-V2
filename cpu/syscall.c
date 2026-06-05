#include "syscall.h"
#include "idt.h"
#include "../drivers/vga.h"
#include "../drivers/serial.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../mm/kheap.h"
#include "../process/process.h"
#include "../process/scheduler.h"
#include "../lib/printf.h"
#include "../fs/vfs.h"
#include "../shell/shell.h"

extern void syscall_stub(void);
extern void enter_usermode(uint32_t entry, uint32_t user_stack);
extern void tss_set_kernel_stack(uint32_t esp0);

static bool g_in_usermode;        /* unused now but kept for sig */

/* ---- ELF exit jump buffer ----
 *
 * `elf_exec()` runs an ELF as a plain in-kernel function call (it has no
 * per-process address space yet). For the ELF to be able to terminate
 * cleanly it would need to either:
 *   (a) return from main(), then unwind through the stub, then `ret` —
 *       but `ret` lands on garbage left by the shell at the top of the
 *       user stack, and
 *   (b) call SYS_EXIT, but a `schedule()` from inside the syscall would
 *       context-switch away from the shell forever.
 *
 * Solution: elf_exec() registers a longjmp landing here.  SYS_EXIT (and
 * usermode_fault_kill, used by CPU exceptions) restore that context,
 * which makes the kernel pop back to elf_exec() right after fn(). */
typedef struct {
    uint32_t valid;
    uint32_t esp, ebp, ebx, esi, edi, eip;
    int      exit_code;
} elf_jmp_t;

static elf_jmp_t g_elf_jmp;

/* Implemented in elf_jmp.asm — saves the callee-saved registers + the
 * address of the instruction right after this call into `dst`, returns 0.
 * elf_jmp_longjmp() restores them, with the saved EIP becoming the
 * "return address" of elf_jmp_setjmp() and returning a non-zero value. */
extern int  elf_jmp_setjmp (elf_jmp_t *dst);
extern void elf_jmp_longjmp(elf_jmp_t *src) __attribute__((noreturn));

void elf_arm_exit_jmp(void)
{
    g_elf_jmp.valid = 0;
    if (elf_jmp_setjmp(&g_elf_jmp) == 0)
        g_elf_jmp.valid = 1;
}

int elf_get_exit_code(void)
{
    return g_elf_jmp.exit_code;
}

void elf_disarm_exit_jmp(void)
{
    g_elf_jmp.valid = 0;
}

bool usermode_fault_kill(int signal_code)
{
    process_exit(signal_code);
    if (g_elf_jmp.valid) {
        g_elf_jmp.exit_code = signal_code;
        g_elf_jmp.valid     = 0;
        elf_jmp_longjmp(&g_elf_jmp);   /* never returns */
    }
    /* No ELF context to unwind to — fall back to legacy behaviour. */
    schedule();
    return true;
}

void syscall_install(void)
{
    idt_set_gate(0x80, (uint32_t)syscall_stub, 0x08, 0xEE);
}

void syscall_handler(registers_t *regs)
{
    uint32_t num = regs->eax;
    uint32_t a1  = regs->ebx;
    uint32_t a2  = regs->ecx;
    uint32_t a3  = regs->edx;

    switch (num) {
    case SYS_EXIT:
        /* Mark the current task as zombie and either:
         *   - longjmp back to elf_exec() if it armed an exit point
         *     (this is the path tcc-compiled programs take), or
         *   - call schedule() so the kernel-thread runner picks the
         *     next task (this is the path real kthreads take).
         * In both cases we never return to the caller of int 0x80. */
        process_exit((int)a1);
        if (g_elf_jmp.valid) {
            g_elf_jmp.exit_code = (int)a1;
            g_elf_jmp.valid     = 0;
            elf_jmp_longjmp(&g_elf_jmp);   /* never returns */
        }
        schedule();
        break;

    case SYS_WRITE: {
        const char *buf = (const char *)a2;
        uint32_t len = a3;
        for (uint32_t i = 0; i < len; i++) {
            vga_putchar(buf[i]);
            serial_write_char(buf[i]);
        }
        regs->eax = len;            
        break;
    }

    case SYS_GETPID:
        regs->eax = process_get_current()->pid;
        break;

    case SYS_YIELD:
        schedule();
        regs->eax = 0;
        break;

    case SYS_SLEEP:
        sleep(a1); /* note: sleep blocks! ideally should block process. */
        regs->eax = 0;
        break;

    case SYS_GETC:
        regs->eax = (uint32_t)keyboard_getchar();
        break;

    case SYS_UPTIME:
        regs->eax = uptime();
        break;

    case SYS_READFILE: {
        /* ebx = path, ecx = buf, edx = max bytes; returns bytes read or -1 */
        const char *path  = (const char *)a1;
        uint8_t    *buf   = (uint8_t    *)a2;
        uint32_t    maxsz = a3;
        shell_state_t *s  = shell_get_state();
        vfs_node_t *n = vfs_resolve(path, s ? s->cwd : 0);
        if (!n || n->type != VFS_FILE) { regs->eax = (uint32_t)-1; break; }
        uint32_t want = n->size < maxsz ? n->size : maxsz;
        regs->eax = vfs_read(n, 0, want, buf);
        break;
    }

    case SYS_WRITEFILE: {
        /* ebx = path, ecx = buf, edx = len; returns bytes written or -1.
         * Creates the file if it doesn't exist; truncates if it does. */
        const char *path = (const char *)a1;
        uint8_t    *buf  = (uint8_t    *)a2;
        uint32_t    len  = a3;
        shell_state_t *s = shell_get_state();
        vfs_node_t *n = vfs_create(path, s ? s->cwd : 0);
        if (!n) { regs->eax = (uint32_t)-1; break; }
        n->size = 0;
        regs->eax = vfs_write(n, 0, len, buf);
        break;
    }

    case SYS_GETLINE: {
        /* ebx = buf, ecx = max; reads from the keyboard until '\n', echoes
         * characters as it goes, supports basic backspace.  Returns the
         * length written (not counting the terminating NUL). */
        char    *buf = (char *)a1;
        uint32_t max = a2;
        if (max == 0) { regs->eax = 0; break; }
        regs->eax = (uint32_t)keyboard_readline(buf, (int)max);
        break;
    }

    default:
        kprintf("\n[syscall] unknown syscall %u\n", num);
        regs->eax = (uint32_t)-1;
        break;
    }
}

static void usermode_trampoline(void) {
    process_t* p = process_get_current();
    uint8_t *ustack = (uint8_t *)kmalloc_aligned(8192);
    uint32_t user_stack_top = (uint32_t)(ustack + 8192) & ~0xF;
    
    enter_usermode((uint32_t)p->entry, user_stack_top);
    /* never returns */
}

int usermode_run(const char *name, void (*entry)(void))
{
    process_t *p = process_create(name, usermode_trampoline);
    if (!p) return -1;
    p->entry = entry; /* store target */
    scheduler_add(p);
    return p->pid; /* now returns pid! */
}
