#ifndef CPU_ISR_H
#define CPU_ISR_H

#include "idt.h"

typedef void (*isr_t)(registers_t *);

void isr_install(void);
void isr_register_handler(uint8_t n, isr_t handler);
void isr_handler(registers_t *regs);

#endif /* CPU_ISR_H */
