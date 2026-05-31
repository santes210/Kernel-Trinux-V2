#ifndef CPU_IDT_H
#define CPU_IDT_H

#include "../lib/types.h"

/* Register frame pushed by the ISR/IRQ assembly stubs. */
typedef struct registers {
    uint32_t ds;                                     /* data segment */
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; /* pusha */
    uint32_t int_no, err_code;                       /* pushed by stub */
    uint32_t eip, cs, eflags, useresp, ss;           /* pushed by CPU */
} registers_t;

#define IDT_ENTRIES 256

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
void idt_install(void);

#endif /* CPU_IDT_H */
