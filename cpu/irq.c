/* cpu/irq.c  -  Hardware IRQ handling + 8259 PIC remap. */
#include "irq.h"
#include "idt.h"
#include "ports.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

static irq_handler_t irq_routines[16];

/* Remap the PIC so IRQ 0-15 map to interrupts 32-47 (avoids clashing with
 * CPU exceptions). */
static void pic_remap(void)
{
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11); io_wait();   /* start init, expect ICW4 */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();  /* master offset 0x20 (32) */
    outb(PIC2_DATA, 0x28); io_wait();  /* slave offset 0x28 (40) */
    outb(PIC1_DATA, 0x04); io_wait();  /* tell master slave at IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* tell slave its cascade identity */
    outb(PIC1_DATA, 0x01); io_wait();  /* 8086/88 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, a1);               /* restore masks */
    outb(PIC2_DATA, a2);
}

void irq_install(void)
{
    pic_remap();

    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);
}

void irq_register_handler(uint8_t irq, irq_handler_t handler)
{
    if (irq < 16)
        irq_routines[irq] = handler;
}

void irq_unregister_handler(uint8_t irq)
{
    if (irq < 16)
        irq_routines[irq] = 0;
}

/* Disable (mask) one IRQ line.  Setting the bit in the PIC IMR blocks the
 * line from raising an interrupt. */
void irq_set_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (uint8_t)(irq - 8);
    uint8_t  m    = inb(port);
    outb(port, (uint8_t)(m | (1u << bit)));
}

/* Enable (unmask) one IRQ line.  Clearing the bit in the PIC IMR lets the
 * line raise interrupts again.  Must be called for every IRQ a driver
 * wants to actually receive, because BIOS/GRUB normally leave most lines
 * masked (notably IRQ4 = COM1, IRQ3 = COM2). */
void irq_clear_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (uint8_t)(irq - 8);
    uint8_t  m    = inb(port);
    outb(port, (uint8_t)(m & ~(1u << bit)));
    /* If unmasking a slave-PIC line, make sure the cascade (IRQ2) on the
     * master is also unmasked, otherwise nothing comes through. */
    if (irq >= 8) {
        uint8_t mm = inb(PIC1_DATA);
        outb(PIC1_DATA, (uint8_t)(mm & ~(1u << 2)));
    }
}

void irq_handler(registers_t *regs)
{
    int irq = regs->int_no - 32;

    /* Send End-Of-Interrupt: to slave first if IRQ >= 8, then master.
     * We do this BEFORE the handler so that if the handler does a 
     * context switch, the new task still receives interrupts! */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);

    if (irq >= 0 && irq < 16 && irq_routines[irq])
        irq_routines[irq](regs);
}
