/* cpu/syscall.c  -  int 0x80 system-call layer + ring-3 program runner.
 *
 * This brings the "big jump" from a ring-0-only hobby kernel to one that can
 * run an unprivileged userspace program that can ONLY touch the machine
 * through syscalls. The demo flow is:
 *
 *   usermode_run(name, entry)
 *       -> creates a process table entry, allocates a user stack,
 *          loads the TSS kernel stack, then drops to ring 3 at `entry`.
 *   user code runs in ring 3 (CS=0x1b) and calls `int 0x80` for everything.
 *   SYS_EXIT longjmps back into usermode_run(), which returns the exit code.
 */
#include "syscall.h"
#include "idt.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../mm/kheap.h"
#include "../process/process.h"
#include "../process/scheduler.h"
#include "../lib/printf.h"

extern void syscall_stub(void);
extern int  usermode_save_and_enter(uint32_t entry, uint32_t user_stack,
                                    uint32_t *save_esp);
extern void usermode_exit_resume(uint32_t saved_esp, int code);
extern void tss_set_kernel_stack(uint32_t esp0);

/* State for the currently running ring-3 program (single-program demo). */
static uint32_t g_saved_kernel_esp;   /* where to longjmp back to on exit   */
static bool     g_in_usermode;        /* are we inside a ring-3 program?     */
static uint32_t g_user_pid;           /* pid of the running user program     */

/* Called by the exception handler when a CPU fault happens while a ring-3
 * program is running. Instead of panicking the whole kernel, we kill the user
 * program and resume in the kernel (the program "crashed"). Returns true if it
 * handled it (does not return in that case), false if no user program is up. */
bool usermode_fault_kill(int signal_code)
{
    if (!g_in_usermode)
        return false;
    g_in_usermode = false;
    usermode_exit_resume(g_saved_kernel_esp, signal_code);
    return true;   /* unreachable */
}

void syscall_install(void)
{
    /* 0xEE = present, DPL 3, 32-bit interrupt gate. DPL 3 lets ring-3 code
     * issue `int 0x80` without a General Protection Fault. */
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
        /* Return control to usermode_run(). Does not come back here. */
        g_in_usermode = false;
        usermode_exit_resume(g_saved_kernel_esp, (int)a1);
        /* unreachable */
        break;

    case SYS_WRITE: {
        /* a1=fd (ignored, everything goes to the screen), a2=buf, a3=len */
        const char *buf = (const char *)a2;
        uint32_t len = a3;
        for (uint32_t i = 0; i < len; i++)
            vga_putchar(buf[i]);
        regs->eax = len;            /* bytes "written" */
        break;
    }

    case SYS_GETPID:
        regs->eax = g_user_pid;
        break;

    case SYS_YIELD:
        /* Cooperative: let the round-robin scheduler pick someone (no-op when
         * the run queue is empty, which is the common case for the demo). */
        schedule();
        regs->eax = 0;
        break;

    case SYS_SLEEP:
        sleep(a1);
        regs->eax = 0;
        break;

    case SYS_GETC:
        regs->eax = (uint32_t)keyboard_getchar();
        break;

    case SYS_UPTIME:
        regs->eax = uptime();
        break;

    default:
        kprintf("\n[syscall] unknown syscall %u\n", num);
        regs->eax = (uint32_t)-1;
        break;
    }
}

#define USER_STACK_SIZE 8192

int usermode_run(const char *name, void (*entry)(void))
{
    if (g_in_usermode)
        return -1;   /* no nested user programs in this simple demo */

    /* Allocate a ring-3 stack (page-aligned for cleanliness). */
    uint8_t *ustack = (uint8_t *)kmalloc_aligned(USER_STACK_SIZE);
    if (!ustack)
        return -2;
    uint32_t user_stack_top = (uint32_t)(ustack + USER_STACK_SIZE);
    user_stack_top &= ~0xF;   /* 16-byte align */

    /* Allocate a kernel stack the CPU will switch to on every ring3->ring0
     * trap (syscalls, IRQs). It must be valid for the whole user run. */
    uint8_t *kstack = (uint8_t *)kmalloc_aligned(USER_STACK_SIZE);
    if (!kstack) { kfree(ustack); return -2; }
    tss_set_kernel_stack((uint32_t)(kstack + USER_STACK_SIZE));

    /* Register the program in the process table so ps/top show it. */
    process_t *p = process_create(name, entry);
    if (p) { p->state = PROC_RUNNING; g_user_pid = p->pid; }
    else   { g_user_pid = 0; }

    g_in_usermode = true;
    int code = usermode_save_and_enter((uint32_t)entry, user_stack_top,
                                       &g_saved_kernel_esp);
    /* back in ring 0 after SYS_EXIT */
    g_in_usermode = false;
    if (p) p->state = PROC_ZOMBIE;

    kfree(ustack);
    kfree(kstack);
    return code;
}
