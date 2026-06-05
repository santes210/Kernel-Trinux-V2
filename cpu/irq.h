#ifndef CPU_IRQ_H
#define CPU_IRQ_H

#include "idt.h"

typedef void (*irq_handler_t)(registers_t *);

void irq_install(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);
void irq_unregister_handler(uint8_t irq);
void irq_handler(registers_t *regs);

/* Unmask (enable) / mask (disable) a single IRQ line on the 8259 PIC.
 * BIOS/GRUB normally hand off with several lines (e.g. IRQ4 = serial)
 * masked, so any driver that registers an IRQ must also unmask it. */
void irq_set_mask(uint8_t irq);
void irq_clear_mask(uint8_t irq);

#endif /* CPU_IRQ_H */
