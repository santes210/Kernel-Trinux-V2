#ifndef CPU_IRQ_H
#define CPU_IRQ_H

#include "idt.h"

typedef void (*irq_handler_t)(registers_t *);

void irq_install(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);
void irq_unregister_handler(uint8_t irq);
void irq_handler(registers_t *regs);

#endif /* CPU_IRQ_H */
