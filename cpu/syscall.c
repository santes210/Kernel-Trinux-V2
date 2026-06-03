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

extern void syscall_stub(void);
extern void enter_usermode(uint32_t entry, uint32_t user_stack);
extern void tss_set_kernel_stack(uint32_t esp0);

static bool g_in_usermode;        /* unused now but kept for sig */

bool usermode_fault_kill(int signal_code)
{
    process_exit(signal_code);
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
        process_exit((int)a1);
        schedule(); /* never returns */
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
