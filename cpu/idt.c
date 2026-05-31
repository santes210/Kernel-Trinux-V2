/* cpu/idt.c  -  Interrupt Descriptor Table. */
#include "idt.h"
#include "../lib/string.h"

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;          /* code segment selector */
    uint8_t  always0;
    uint8_t  flags;        /* type & attributes */
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

extern void idt_flush(uint32_t);

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

void idt_install(void)
{
    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base  = (uint32_t)&idt;
    memset(&idt, 0, sizeof(idt));
    idt_flush((uint32_t)&idtp);
}
