/* boot/gdt.c  -  Global Descriptor Table setup. */
#include "../lib/types.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* The 32-bit Task State Segment. We only use it to tell the CPU which kernel
 * stack to switch to (ss0:esp0) when a ring-3 task traps into ring 0 via an
 * interrupt/syscall. Everything else is left zero. */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;       /* kernel stack pointer loaded on ring3->ring0 */
    uint32_t ss0;        /* kernel stack segment */
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

#define GDT_ENTRIES 6   /* null, k-code, k-data, u-code, u-data, TSS */

static struct gdt_entry  gdt[GDT_ENTRIES];
static struct gdt_ptr    gp;
static struct tss_entry  tss;

extern void gdt_flush(uint32_t);
extern void tss_flush(void);   /* loads TR with the TSS selector (0x28) */

static void gdt_set_gate(int num, uint32_t base, uint32_t limit,
                         uint8_t access, uint8_t gran)
{
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access      = access;
}

/* Install GDT entry #5 as the TSS descriptor. */
static void write_tss(int num, uint16_t ss0, uint32_t esp0)
{
    uint32_t base  = (uint32_t)&tss;
    uint32_t limit = sizeof(struct tss_entry) - 1;

    /* access 0x89 = present, type 0x9 (32-bit TSS available), DPL 0 */
    gdt_set_gate(num, base, limit, 0x89, 0x00);

    /* zero the TSS, then set the kernel stack the CPU loads on a ring switch */
    for (uint32_t i = 0; i < sizeof(struct tss_entry); i++)
        ((uint8_t *)&tss)[i] = 0;
    tss.ss0  = ss0;     /* kernel data selector (0x10) */
    tss.esp0 = esp0;    /* top of the kernel stack for ring3 traps */
    /* segment selectors a ring3 task will use when it traps in (RPL 3) */
    tss.cs = 0x0b;
    tss.ss = tss.ds = tss.es = tss.fs = tss.gs = 0x13;
    tss.iomap_base = sizeof(struct tss_entry);
}

/* Update esp0 (called before dropping to ring 3, and on each context switch
 * into a user task) so the next ring3->ring0 trap lands on the right stack. */
void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}

/* Called from boot.asm before kernel_main. */
void gdt_install(void)
{
    gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gp.base  = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);                     /* null */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);      /* code: ring0 exec/read */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);      /* data: ring0 read/write */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);      /* user code (ring3) */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);      /* user data (ring3) */

    /* TSS at entry 5 (selector 0x28). A temporary esp0 is set here; the real
     * per-task kernel stack is loaded with tss_set_kernel_stack() before each
     * ring-3 entry. */
    write_tss(5, 0x10, 0x0);

    gdt_flush((uint32_t)&gp);
    tss_flush();
}

