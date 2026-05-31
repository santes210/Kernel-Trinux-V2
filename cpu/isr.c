/* cpu/isr.c  -  CPU exception handlers (ISR 0-31). */
#include "isr.h"
#include "idt.h"
#include "syscall.h"
#include "../lib/printf.h"
#include "../drivers/vga.h"
#include "../drivers/serial.h"
#include "../include/kernel.h"

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

static isr_t handlers[32];

static const char *exception_messages[32] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved"
};

void isr_install(void)
{
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);
}

void isr_register_handler(uint8_t n, isr_t handler)
{
    if (n < 32)
        handlers[n] = handler;
}

static void dump_registers(registers_t *r)
{
    kprintf("  eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
            r->eax, r->ebx, r->ecx, r->edx);
    kprintf("  esi=%08x edi=%08x ebp=%08x esp=%08x\n",
            r->esi, r->edi, r->ebp, r->esp);
    kprintf("  eip=%08x cs=%04x  eflags=%08x ds=%04x\n",
            r->eip, r->cs, r->eflags, r->ds);
    kprintf("  int_no=%u err_code=%08x\n", r->int_no, r->err_code);
}

void isr_handler(registers_t *regs)
{
    if (regs->int_no < 32 && handlers[regs->int_no]) {
        handlers[regs->int_no](regs);
        return;
    }

    vga_set_color(vga_entry_color(VGA_WHITE, VGA_RED));
    kprintf("\n*** CPU EXCEPTION ***\n");
    kprintf("  %s (interrupt %u)\n",
            exception_messages[regs->int_no & 31], regs->int_no);
    vga_set_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    dump_registers(regs);

    serial_printf("\n*** CPU EXCEPTION *** %s (int %u) err=%x eip=%x cs=%x esp=%x ss=%x\n",
                  exception_messages[regs->int_no & 31], regs->int_no,
                  regs->err_code, regs->eip, regs->cs, regs->useresp, regs->ss);

    /* If the fault came from ring 3 (a user program), don't bring down the
     * whole kernel — terminate the offending program and return to the shell.
     * (cs RPL == 3 means the faulting code was unprivileged.) */
    if ((regs->cs & 3) == 3) {
        kprintf("  (terminating ring-3 program; kernel continues)\n");
        if (usermode_fault_kill(-(int)regs->int_no))
            return;   /* unreachable: resumes in the kernel */
    }

    panic("Unhandled CPU exception");
}
