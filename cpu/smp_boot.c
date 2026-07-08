/* cpu/smp_boot.c — Arranque de Application Processors via LAPIC IPI.
 *
 * CAMBIO #6: convierte los cores detectados (pero offline) en cores
 * realmente ejecutando código del kernel de Trinux.
 *
 * Flujo completo:
 *   BSP llama smp_boot_aps()
 *     └─ para cada AP (apic_id != bsp_id):
 *         1. Copiar trampoline a 0x8000 (real-mode entry point)
 *         2. Escribir dirección de la GDT y page directory en la
 *            zona de parámetros del trampoline (últimas 8 words)
 *         3. Enviar INIT IPI → esperar 10 ms
 *         4. Enviar STARTUP IPI × 2 → esperar 1 ms
 *         5. Polling en ap_booted[ap_id] hasta que el AP lo ponga a 1
 *            (o timeout de 200 ms)
 *
 * El trampoline (16-bit real mode → 32-bit protected mode) está codificado
 * como bytes en este archivo para evitar dependencias de compilación extra.
 *
 * Referencia: Intel IA-32 SDM Vol. 3, §8.4 "MP Initialization Protocol"
 */
#include "smp_boot.h"
#include "smp.h"
#include "ports.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/timer.h"
#include "../mm/kheap.h"
#include "../mm/vmm.h"
#include "../process/scheduler.h"

/* ---- Acceso al LAPIC ---- */
/* El LAPIC está mapeado en memoria en smp_lapic_base().
 * Accedemos via puntero volatile a uint32_t. */

static volatile uint32_t *lapic_base_ptr(void)
{
    uint32_t base = smp_lapic_base();
    if (!base) return NULL;
    /* Asegurar que está mapeado (puede estar > 256 MiB) */
    extern void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
    vmm_map_page(base & ~0xFFFu, base & ~0xFFFu,
                 0x3 | 0x10);   /* PRESENT | RW | CACHE_DISABLE */
    return (volatile uint32_t *)base;
}

#define LAPIC_ID         0x020   /* Local APIC ID register (offset en bytes) */
#define LAPIC_ICR_LO     0x300   /* Interrupt Command Register (low)  */
#define LAPIC_ICR_HI     0x310   /* Interrupt Command Register (high) */
#define LAPIC_SVR        0x0F0   /* Spurious Vector Register */

static void lapic_write(uint32_t offset, uint32_t value)
{
    volatile uint32_t *lapic = lapic_base_ptr();
    if (!lapic) return;
    lapic[offset / 4] = value;
    /* Memory barrier: asegurar que el write llegó al LAPIC */
    __asm__ volatile("" ::: "memory");
}

static uint32_t lapic_read(uint32_t offset)
{
    volatile uint32_t *lapic = lapic_base_ptr();
    if (!lapic) return 0;
    return lapic[offset / 4];
}

/* Espera a que el IPI delivery esté completo (bit 12 del ICR_LO = 0). */
static void lapic_wait_icr(void)
{
    for (int i = 0; i < 10000; i++) {
        if (!(lapic_read(LAPIC_ICR_LO) & (1u << 12))) return;
        __asm__ volatile("pause");
    }
}

/* ---- Trampoline en 16-bit real mode ---- */
/*
 * El trampoline hace lo mínimo para pasar de real mode a protected mode:
 *   1. Deshabilita interrupciones (CLI)
 *   2. Carga la GDT del BSP (en dirección fija 0x8000 + offset)
 *   3. Habilita PE en CR0
 *   4. Salto largo a la función ap_entry() en 32 bits
 *
 * Los parámetros (GDT ptr, stack top, cr3, ap_main addr) se pasan como
 * palabras de 32 bits al final del bloque de trampoline.
 *
 * El código es real-mode x86 (16 bits). Lo generamos como bytes para
 * no necesitar un ensamblado separado.
 */

/* Indicador que cada AP pone a 1 cuando está listo. */
static volatile uint8_t ap_booted[SMP_MAX_CPUS];
static int aps_online_count = 0;

/* Stack de kernel para cada AP. */
static uint8_t *ap_stacks[SMP_MAX_APS];

/* Entry point de 32 bits al que salta el trampoline.
 * Se declara extern porque se define en smp_entry.asm (o inline aquí). */
extern void smp_ap_entry(void);

/*
 * Bytes del trampoline (real-mode, base 0x8000):
 * Offset 0x00: CLI
 * Offset 0x01: LGDT [cs:gdt_ptr_offset]  (usa GDT del BSP)
 * Offset 0x07: MOV EAX, CR0; OR AL, 1; MOV CR0, EAX
 * Offset 0x0F: JMP FAR 0x08:ap_entry_32  (salto a 32-bit PM)
 *
 * Al final del bloque (offset 0xF0 .. 0xFF) ponemos los parámetros:
 *   [0xF0] gdt_ptr (6 bytes: limit word + base dword)
 *   [0xF6] ap_entry_32 (4 bytes: dirección de smp_ap_entry)
 *   [0xFA] ap_stack_top (4 bytes: stack del AP)
 *   [0xFE] ap_cr3 (4 bytes: page directory del BSP)
 *
 * NOTA: como Trinux es un kernel educativo de 32 bits, el trampoline
 * usa la GDT y CR3 del BSP directamente (espacio de direcciones compartido).
 */

/* Estructura de parámetros al final del trampoline (en 0x8000 + offsets). */
#define TRAMP_PARAM_GDT    0xF0   /* 6 bytes */
#define TRAMP_PARAM_ENTRY  0xF6   /* 4 bytes */
#define TRAMP_PARAM_STACK  0xFA   /* 4 bytes */
#define TRAMP_PARAM_CR3    0xFE   /* 4 bytes — nota: 0x100 = 256 bytes total */

/* Trampoline en real mode (16 bits). Los offsets asumen base 0x8000. */
static const uint8_t trampoline_code[256] = {
    /* 0x00: cli */
    0xFA,
    /* 0x01: lgdt [0x8000 + TRAMP_PARAM_GDT]
     * En real mode con CS=0x800, DS=0x800, la dirección es 0x800*16+0xF0=0x80F0.
     * Usamos lgdt con dirección absoluta: 0x0F 0x01 0x16 lo hi */
    0x0F, 0x01, 0x16, 0xF0, 0x80,   /* lgdt [0x80F0] — dirección de la GDT en mem baja */
    /* 0x07: mov eax, cr0 */
    0x0F, 0x20, 0xC0,
    /* 0x0A: or al, 1  (set PE) */
    0x0C, 0x01,
    /* 0x0C: mov cr0, eax */
    0x0F, 0x22, 0xC0,
    /* 0x0F: jmp far 0x08:0x00008016 (salto a 32 bits, dirección fija 0x8016) */
    0xEA,
    0x16, 0x80, 0x00, 0x00,   /* offset 0x8016 little-endian */
    0x08, 0x00,               /* segment 0x08 (kernel code) */
    /* 0x16: código de 32 bits — el AP está ahora en protected mode */
    /* A partir de aquí los bytes son i386 (32-bit) */
    /* mov ax, 0x10; mov ds,ax; mov es,ax; mov ss,ax; mov fs,ax; mov gs,ax */
    0x66, 0xB8, 0x10, 0x00,   /* mov ax, 0x10 */
    0x8E, 0xD8,               /* mov ds, ax */
    0x8E, 0xC0,               /* mov es, ax */
    0x8E, 0xD0,               /* mov ss, ax */
    0x8E, 0xE0,               /* mov fs, ax */
    0x8E, 0xE8,               /* mov gs, ax */
    /* Cargar stack: mov esp, [0x80FA] */
    0x8B, 0x25, 0xFA, 0x80, 0x00, 0x00,   /* mov esp, [0x80FA] */
    /* Cargar CR3: mov eax,[0x80FE]; mov cr3,eax */
    0xA1, 0xFE, 0x80, 0x00, 0x00,         /* mov eax, [0x80FE] */
    0x0F, 0x22, 0xD8,                      /* mov cr3, eax */
    /* Habilitar paginación (CR0.PG ya estaba en la GDT del BSP): no-op aquí */
    /* Saltar a la función ap_main del kernel: jmp [0x80F6] */
    0xFF, 0x25, 0xF6, 0x80, 0x00, 0x00,   /* jmp dword [0x80F6] */
    /* Relleno hasta 0xF0 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0xF0: GDT ptr (6 bytes) — se rellena en smp_boot_aps() */
    0, 0, 0, 0, 0, 0,
    /* 0xF6: ap_entry (4 bytes) */
    0, 0, 0, 0,
    /* 0xFA: stack top (4 bytes) */
    0, 0, 0, 0,
    /* 0xFE: cr3 (4 bytes) */
    0, 0, 0, 0
};

/* Entry point C para los APs — llamado desde el trampoline. */
void ap_main(uint32_t ap_id)
{
    /* Marcar que este AP arrancó */
    if (ap_id < SMP_MAX_CPUS)
        ap_booted[ap_id] = 1;
    aps_online_count++;

    kprintf("  [SMP] AP %u online\n", ap_id);

    /* El AP entra al scheduler loop. En Trinux actual el scheduler
     * es cooperativo y single-threaded, así que los APs simplemente
     * hacen HLT hasta que el scheduler les asigne trabajo. */
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

/* ---- Envío de IPIs ---- */

static void lapic_send_init(uint8_t dest_apic_id)
{
    lapic_write(LAPIC_ICR_HI, (uint32_t)dest_apic_id << 24);
    lapic_write(LAPIC_ICR_LO, 0x00004500);   /* INIT, level assert */
    lapic_wait_icr();
    sleep(10);   /* 10 ms según el spec */
    lapic_write(LAPIC_ICR_LO, 0x00008500);   /* INIT, level deassert */
    lapic_wait_icr();
}

static void lapic_send_startup(uint8_t dest_apic_id, uint8_t vector)
{
    lapic_write(LAPIC_ICR_HI, (uint32_t)dest_apic_id << 24);
    lapic_write(LAPIC_ICR_LO, 0x00004600 | vector);   /* SIPI */
    lapic_wait_icr();
    sleep(1);    /* 1 ms */
}

/* ---- API pública ---- */

int smp_boot_aps(void)
{
    int n_cpus = smp_cpu_count();
    if (n_cpus <= 1) return 0;   /* solo BSP, nada que arrancar */

    uint8_t bsp_id = smp_bsp_apic_id();

    /* Obtener dirección del CR3 y del descriptor de la GDT del BSP */
    uint32_t bsp_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(bsp_cr3));

    /* Estructura de 6 bytes para LGDT: { limit:u16, base:u32 } */
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) gdt_ptr;
    __asm__ volatile("sgdt %0" : "=m"(gdt_ptr));

    int ap_idx = 0;
    int booted = 0;

    for (int i = 0; i < n_cpus; i++) {
        smp_cpu_t *cpu = smp_cpu_at(i);
        if (!cpu || cpu->apic_id == bsp_id) continue;
        if (ap_idx >= SMP_MAX_APS) break;

        /* Alocar stack de kernel para el AP */
        ap_stacks[ap_idx] = (uint8_t *)kmalloc(AP_STACK_SIZE);
        if (!ap_stacks[ap_idx]) {
            kprintf("[SMP] no RAM for AP %u stack\n", cpu->apic_id);
            ap_idx++;
            continue;
        }
        uint32_t stack_top = (uint32_t)ap_stacks[ap_idx] + AP_STACK_SIZE;

        /* Copiar trampoline a 0x8000 */
        memcpy((void *)AP_TRAMPOLINE_ADDR, trampoline_code, 256);

        /* Rellenar parámetros al final del trampoline */
        uint8_t *tramp = (uint8_t *)AP_TRAMPOLINE_ADDR;
        memcpy(tramp + TRAMP_PARAM_GDT,   &gdt_ptr,        6);
        memcpy(tramp + TRAMP_PARAM_ENTRY, &ap_main,        4);
        memcpy(tramp + TRAMP_PARAM_STACK, &stack_top,      4);
        memcpy(tramp + TRAMP_PARAM_CR3,   &bsp_cr3,        4);

        ap_booted[cpu->apic_id] = 0;

        kprintf("  [SMP] booting AP %u (APIC ID %u)...\n",
                ap_idx, cpu->apic_id);

        /* INIT IPI */
        lapic_send_init(cpu->apic_id);

        /* STARTUP IPI × 2 (vector = trampoline >> 12 = 0x08) */
        uint8_t sipi_vec = (uint8_t)(AP_TRAMPOLINE_ADDR >> 12);
        lapic_send_startup(cpu->apic_id, sipi_vec);
        lapic_send_startup(cpu->apic_id, sipi_vec);

        /* Esperar a que el AP confirme el boot (polling, timeout 200 ms) */
        for (int t = 0; t < 200; t++) {
            if (ap_booted[cpu->apic_id]) { booted++; break; }
            sleep(1);
        }
        if (!ap_booted[cpu->apic_id])
            kprintf("  [SMP] AP %u timeout — posiblemente no soportado\n",
                    cpu->apic_id);

        ap_idx++;
    }

    return booted;
}

int smp_aps_online(void) { return aps_online_count; }
