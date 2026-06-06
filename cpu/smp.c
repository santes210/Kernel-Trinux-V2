/* cpu/smp.c — detección SMP via ACPI MADT.
 *
 * Algoritmo:
 *   1. Buscar la firma "RSD PTR " en BIOS data area (EBDA o 0xE0000-0xFFFFF).
 *      Eso es el RSDP (Root System Description Pointer).
 *   2. Leer el RSDT desde el RSDP. El RSDT lista todas las tablas ACPI.
 *   3. Recorrer el RSDT buscando una entrada con firma "APIC" — esa es la MADT.
 *   4. Recorrer las entradas de la MADT:
 *      - Type 0 = LAPIC: un core. Lo añadimos a smp_cpus[].
 *      - Type 1 = IOAPIC: ignoramos (no necesitamos por ahora).
 *      - Type 5 = LAPIC override: dirección del LAPIC (típicamente 0xFEE00000).
 *
 * Si en algún paso fallamos (firmas no encontradas, checksum malo, addr fuera
 * de los 256 MB del identity-map del kernel...), devolvemos 1 core (BSP).
 */
#include "smp.h"
#include "ports.h"
#include "../lib/printf.h"
#include "../drivers/serial.h"

/* vmm_map_page lo necesitamos para mapear tablas ACPI que estan en
 * direcciones altas (>256 MB), tipicamente cerca de 4 GB (0xBFFExxxx
 * en QEMU). Sin esto no podemos leer el RSDT. */
extern void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

/* Mapea identity una pagina si esta fuera del identity-map de 256 MB.
 * No-op si la pagina ya esta mapeada. */
static void map_acpi_page(uint32_t phys)
{
    if (phys < 0x10000000u) return;  /* dentro de los primeros 256 MB ya */
    vmm_map_page(phys & ~0xFFFu, phys & ~0xFFFu, 0x3);
}

/* Mapea un rango (para tablas largas que cruzan paginas). */
static void map_acpi_range(uint32_t phys, uint32_t len)
{
    if (phys < 0x10000000u && phys + len <= 0x10000000u) return;
    uint32_t start = phys & ~0xFFFu;
    uint32_t end = (phys + len + 0xFFF) & ~0xFFFu;
    for (uint32_t p = start; p < end; p += 0x1000) {
        vmm_map_page(p, p, 0x3);
    }
}

/* ---- ACPI structures ---- */

typedef struct {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    /* fields below only for ACPI 2.0+, no nos importan */
} __attribute__((packed)) rsdp_t;

typedef struct {
    char     signature[4];   /* "RSDT", "APIC", etc. */
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct {
    acpi_sdt_header_t header;  /* signature = "RSDT" */
    uint32_t          entries[];  /* array de punteros a otras tablas */
} __attribute__((packed)) rsdt_t;

typedef struct {
    acpi_sdt_header_t header;        /* signature = "APIC" */
    uint32_t          local_apic_addr;
    uint32_t          flags;
    /* entries variable a continuación */
} __attribute__((packed)) madt_t;

/* MADT entry types */
#define MADT_TYPE_LAPIC          0
#define MADT_TYPE_IOAPIC         1
#define MADT_TYPE_INT_OVERRIDE   2
#define MADT_TYPE_LAPIC_OVERRIDE 5

typedef struct {
    uint8_t  type;
    uint8_t  length;
} __attribute__((packed)) madt_entry_header_t;

typedef struct {
    madt_entry_header_t header;
    uint8_t  acpi_id;
    uint8_t  apic_id;
    uint32_t flags;          /* bit 0 = enabled */
} __attribute__((packed)) madt_lapic_t;

typedef struct {
    madt_entry_header_t header;
    uint16_t reserved;
    uint64_t lapic_addr;
} __attribute__((packed)) madt_lapic_override_t;

/* ---- estado ---- */

static smp_cpu_t smp_cpus[SMP_MAX_CPUS];
static int       smp_n_cpus = 0;
static uint32_t  smp_lapic_addr = 0;
static uint8_t   smp_bsp_id = 0;

int       smp_cpu_count(void)        { return smp_n_cpus; }
smp_cpu_t *smp_cpu_at(int i)         { return (i < 0 || i >= smp_n_cpus) ? 0 : &smp_cpus[i]; }
uint32_t  smp_lapic_base(void)       { return smp_lapic_addr; }
uint8_t   smp_bsp_apic_id(void)      { return smp_bsp_id; }
/* Helper para syscall: devuelve apic_id del core i, o 0xFF si no existe. */
uint8_t   smp_get_apic_id_at(int i)  {
    return (i < 0 || i >= smp_n_cpus) ? 0xFF : smp_cpus[i].apic_id;
}

/* Lee el LAPIC ID del core actual. Esto NO requiere haber parseado ACPI —
 * el LAPIC esta mapeado en una dirección fija (típicamente 0xFEE00000) y
 * el registro de ID está en offset 0x20.
 *
 * Pero si LAPIC no está disponible (CPU antiguo sin xAPIC), devolvemos 0.
 * Para saber si hay LAPIC usamos CPUID feature bit. */
static uint8_t read_bsp_apic_id_via_cpuid(void)
{
    uint32_t eax, ebx, ecx, edx;
    /* CPUID leaf 1: feature info. ebx[31:24] = initial APIC ID. */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    /* edx bit 9 = APIC presente */
    if (!(edx & (1u << 9))) return 0xFF;  /* no hay APIC */
    return (uint8_t)(ebx >> 24);
}

/* Suma de bytes para checksum ACPI. */
static uint8_t checksum8(const void *p, uint32_t len)
{
    uint8_t sum = 0;
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < len; i++) sum += b[i];
    return sum;
}

/* Busca "RSD PTR " en una región de memoria. Devuelve puntero o NULL. */
static rsdp_t *find_rsdp_in(uint32_t start, uint32_t end)
{
    /* RSDP esta alineado a 16 bytes */
    for (uint32_t addr = start; addr < end; addr += 16) {
        const char *p = (const char *)addr;
        if (p[0]=='R' && p[1]=='S' && p[2]=='D' && p[3]==' ' &&
            p[4]=='P' && p[5]=='T' && p[6]=='R' && p[7]==' ') {
            rsdp_t *r = (rsdp_t *)addr;
            if (checksum8(r, 20) == 0) {  /* ACPI 1.0 = primeros 20 bytes */
                return r;
            }
        }
    }
    return 0;
}

static rsdp_t *find_rsdp(void)
{
    /* Locación 1: EBDA (Extended BIOS Data Area). El puntero al EBDA está
     * en 0x40E (word) — la dirección está en unidades de 16 bytes. */
    uint16_t ebda_seg = *(volatile uint16_t *)0x40E;
    if (ebda_seg) {
        uint32_t ebda = (uint32_t)ebda_seg << 4;
        if (ebda >= 0x80000 && ebda < 0xA0000) {
            rsdp_t *r = find_rsdp_in(ebda, ebda + 1024);
            if (r) return r;
        }
    }
    /* Locación 2: zona de BIOS extension ROM 0xE0000-0xFFFFF */
    return find_rsdp_in(0xE0000, 0x100000);
}

/* Parsea la MADT y rellena smp_cpus[] + smp_lapic_addr. */
static void parse_madt(madt_t *madt)
{
    smp_lapic_addr = madt->local_apic_addr;

    uint8_t *p = (uint8_t *)(madt + 1);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (p < end) {
        madt_entry_header_t *h = (madt_entry_header_t *)p;
        if (h->length == 0) break;  /* paranoia */

        switch (h->type) {
        case MADT_TYPE_LAPIC: {
            madt_lapic_t *e = (madt_lapic_t *)p;
            /* flag bit 0 = enabled. Si está 0, el core existe pero está
             * permanently disabled (no podemos arrancarlo). */
            if ((e->flags & 1) && smp_n_cpus < SMP_MAX_CPUS) {
                smp_cpus[smp_n_cpus].apic_id = e->apic_id;
                smp_cpus[smp_n_cpus].acpi_id = e->acpi_id;
                smp_cpus[smp_n_cpus].enabled = true;
                smp_cpus[smp_n_cpus].online  = (e->apic_id == smp_bsp_id);
                smp_n_cpus++;
            }
            break;
        }
        case MADT_TYPE_LAPIC_OVERRIDE: {
            madt_lapic_override_t *e = (madt_lapic_override_t *)p;
            /* Si está en addr alta (>4GB) no podemos usarla en x86_32. */
            if ((e->lapic_addr >> 32) == 0) {
                smp_lapic_addr = (uint32_t)e->lapic_addr;
            }
            break;
        }
        default:
            break;
        }
        p += h->length;
    }
}

int smp_detect(void)
{
    /* Inicializar a "1 core, sin SMP". Si todo falla, devolvemos esto. */
    smp_n_cpus = 0;
    smp_lapic_addr = 0;
    smp_bsp_id = read_bsp_apic_id_via_cpuid();
    if (smp_bsp_id == 0xFF) {
        /* CPU sin APIC. Pre-Pentium. Solo modo PIC antiguo. */
        serial_write("[smp] CPU has no APIC; staying single-core\n");
        smp_cpus[0].apic_id = 0;
        smp_cpus[0].acpi_id = 0;
        smp_cpus[0].enabled = true;
        smp_cpus[0].online  = true;
        smp_n_cpus = 1;
        return 1;
    }

    /* Buscar RSDP. */
    rsdp_t *rsdp = find_rsdp();
    if (!rsdp) {
        serial_write("[smp] no RSDP found; staying single-core\n");
        goto fallback_single;
    }

    /* RSDT — en QEMU esta tipicamente en ~0xBFFExxxx (= cerca de 4 GB),
     * fuera del identity-map de 256 MB del kernel. Lo mapeamos identity
     * con vmm_map_page. */
    uint32_t rsdt_addr = rsdp->rsdt_addr;
    if (rsdt_addr == 0) {
        serial_write("[smp] RSDT addr is 0\n");
        goto fallback_single;
    }
    /* Mapeo la primera pagina para poder leer el header (length). */
    map_acpi_page(rsdt_addr);
    rsdt_t *rsdt = (rsdt_t *)rsdt_addr;
    if (rsdt->header.signature[0]!='R' || rsdt->header.signature[1]!='S' ||
        rsdt->header.signature[2]!='D' || rsdt->header.signature[3]!='T') {
        serial_write("[smp] RSDT bad signature\n");
        goto fallback_single;
    }
    /* La tabla puede cruzar varias paginas — mapearlas todas. */
    map_acpi_range(rsdt_addr, rsdt->header.length);
    if (checksum8(rsdt, rsdt->header.length) != 0) {
        serial_write("[smp] RSDT bad checksum\n");
        goto fallback_single;
    }

    /* Recorrer entradas del RSDT buscando MADT ("APIC"). */
    int n_entries = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / 4;
    madt_t *madt = 0;
    for (int i = 0; i < n_entries; i++) {
        uint32_t addr = rsdt->entries[i];
        if (addr == 0) continue;
        map_acpi_page(addr);  /* mapear primera pagina de la tabla */
        acpi_sdt_header_t *h = (acpi_sdt_header_t *)addr;
        if (h->signature[0]=='A' && h->signature[1]=='P' &&
            h->signature[2]=='I' && h->signature[3]=='C') {
            /* Mapear el resto de la tabla por si es larga (>4KB raro pero posible) */
            map_acpi_range(addr, h->length);
            madt = (madt_t *)addr;
            break;
        }
    }
    if (!madt) {
        serial_write("[smp] MADT not found in RSDT\n");
        goto fallback_single;
    }

    parse_madt(madt);

    if (smp_n_cpus == 0) {
        serial_write("[smp] MADT parsed but no enabled CPUs\n");
        goto fallback_single;
    }

    return smp_n_cpus;

fallback_single:
    smp_cpus[0].apic_id = smp_bsp_id;
    smp_cpus[0].acpi_id = 0;
    smp_cpus[0].enabled = true;
    smp_cpus[0].online  = true;
    smp_n_cpus = 1;
    return 1;
}
