/* drivers/acpi_power.c — ACPI power management real.
 *
 * CAMBIO #8: apagado y reset funcionales en hardware real moderno.
 *
 * Parsea el FADT (Fixed ACPI Description Table) para obtener:
 *   - PM1a_CNT_BLK: puerto I/O del control de energía PM1
 *   - PM1b_CNT_BLK: puerto opcional secundario
 *   - SLP_TYPa: tipo de sleep para S5 (apagado)
 *   - RESET_REG: registro de reset (FADT Rev ≥ 2)
 *   - RESET_VALUE: valor a escribir para el reset
 *
 * El parseo del DSDT para encontrar _S5_ se simplifica buscando la
 * secuencia de bytes conocida en el bytecode AML del DSDT.
 */
#include "acpi_power.h"
#include "../cpu/ports.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"

/* ---- Estructuras ACPI ---- */

typedef struct {
    char     sig[4];
    uint32_t length;
    uint8_t  rev;
    uint8_t  checksum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_rev;
    uint32_t creator_id;
    uint32_t creator_rev;
} __attribute__((packed)) acpi_hdr_t;

/* FADT — Fixed ACPI Description Table.
 * Solo los campos que necesitamos para PM y reset. */
typedef struct {
    acpi_hdr_t hdr;           /* 36 bytes */
    uint32_t   facs_addr;
    uint32_t   dsdt_addr;
    uint8_t    int_model;
    uint8_t    preferred_pm;
    uint16_t   sci_int;
    uint32_t   smi_cmd;
    uint8_t    acpi_enable;
    uint8_t    acpi_disable;
    uint8_t    s4bios_req;
    uint8_t    pstate_cnt;
    uint32_t   pm1a_evt_blk;
    uint32_t   pm1b_evt_blk;
    uint32_t   pm1a_cnt_blk;   /* Puerto de control PM1a */
    uint32_t   pm1b_cnt_blk;   /* Puerto de control PM1b (puede ser 0) */
    uint32_t   pm2_cnt_blk;
    uint32_t   pm_tmr_blk;
    uint32_t   gpe0_blk;
    uint32_t   gpe1_blk;
    uint8_t    pm1_evt_len;
    uint8_t    pm1_cnt_len;
    uint8_t    pm2_cnt_len;
    uint8_t    pm_tmr_len;
    uint8_t    gpe0_blk_len;
    uint8_t    gpe1_blk_len;
    uint8_t    gpe1_base;
    uint8_t    cst_cnt;
    uint16_t   p_lvl2_lat;
    uint16_t   p_lvl3_lat;
    uint16_t   flush_size;
    uint16_t   flush_stride;
    uint8_t    duty_offset;
    uint8_t    duty_width;
    uint8_t    day_alrm;
    uint8_t    mon_alrm;
    uint8_t    century;
    uint16_t   iapc_boot_arch;
    uint8_t    reserved2;
    uint32_t   flags;
    /* ACPI 2.0+ (Rev >= 2): reset register */
    uint8_t    reset_reg[12];  /* Generic Address Structure: space_id, bit_width, bit_off, access_size, addr */
    uint8_t    reset_value;
} __attribute__((packed)) fadt_t;

/* ---- Estado ---- */
static bool     acpi_ok       = false;
static uint16_t pm1a_cnt      = 0;
static uint16_t pm1b_cnt      = 0;
static uint16_t slp_typa      = 0;
static uint16_t slp_typb      = 0;
static bool     have_reset_reg = false;
static uint8_t  reset_reg_space;     /* 1 = I/O space */
static uint32_t reset_reg_addr;
static uint8_t  reset_reg_value;

/* ---- Helpers ---- */

static uint8_t acpi_checksum(const void *p, uint32_t len)
{
    uint8_t sum = 0;
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < len; i++) sum += b[i];
    return sum;
}

static void map_page_if_needed(uint32_t addr)
{
    if (addr < 0x10000000u) return;
    extern void vmm_map_page(uint32_t, uint32_t, uint32_t);
    uint32_t page = addr & ~0xFFFu;
    vmm_map_page(page, page, 0x3);
}

/* Busca la secuencia AML de _S5_ en el DSDT.
 * El bytecode AML de "_S5_" es:
 *   { 0x08, '_', 'S', '5', '_', 0x12, ... }
 * Seguido de dos bytes con SLP_TYPa y SLP_TYPb. */
static bool find_s5_in_dsdt(uint32_t dsdt_addr)
{
    if (!dsdt_addr) return false;
    map_page_if_needed(dsdt_addr);

    acpi_hdr_t *hdr = (acpi_hdr_t *)dsdt_addr;
    if (hdr->length < sizeof(acpi_hdr_t)) return false;

    /* Mapear todo el DSDT */
    for (uint32_t off = 0; off < hdr->length; off += 0x1000)
        map_page_if_needed(dsdt_addr + off);

    const uint8_t *data = (const uint8_t *)dsdt_addr;
    uint32_t len = hdr->length;

    /* Buscar la secuencia { 0x08, '_', 'S', '5', '_' } */
    for (uint32_t i = 36; i + 8 < len; i++) {
        if (data[i]   == 0x08 &&
            data[i+1] == '_'  &&
            data[i+2] == 'S'  &&
            data[i+3] == '5'  &&
            data[i+4] == '_') {
            /* Saltar el opcode de Package (puede ser 0x12) */
            uint32_t j = i + 5;
            if (data[j] == 0x12) j++;   /* Package opcode */
            j++;                          /* skip package length */
            if (j + 4 >= len) break;
            if (data[j] != 0x0A) break;  /* debe ser ByteData opcode */
            slp_typa = (uint16_t)(data[j+1] << 10);
            if (data[j+2] == 0x0A) {
                slp_typb = (uint16_t)(data[j+3] << 10);
            } else {
                slp_typb = slp_typa;
            }
            serial_write("[acpi] _S5_ found: SLP_TYPa=");
            serial_write_char('0' + (slp_typa >> 10));
            serial_write("\n");
            return true;
        }
    }
    return false;
}

/* ---- API pública ---- */

bool acpi_power_init(void)
{
    /* Reutilizar el RSDP que smp.c ya sabe encontrar */
    /* Buscamos manualmente aquí también para no acoplarnos */
    const char *rsdp_sig = "RSD PTR ";

    /* Buscar RSDP en EBDA y en 0xE0000-0xFFFFF */
    uint8_t *rsdp_ptr = NULL;
    uint16_t ebda_seg = *(volatile uint16_t *)0x40E;
    if (ebda_seg) {
        uint32_t ebda = (uint32_t)ebda_seg << 4;
        if (ebda >= 0x80000 && ebda < 0xA0000) {
            for (uint32_t a = ebda; a < ebda + 1024; a += 16) {
                if (memcmp((void *)a, rsdp_sig, 8) == 0) {
                    rsdp_ptr = (uint8_t *)a; break;
                }
            }
        }
    }
    if (!rsdp_ptr) {
        for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
            if (memcmp((void *)a, rsdp_sig, 8) == 0) {
                rsdp_ptr = (uint8_t *)a; break;
            }
        }
    }
    if (!rsdp_ptr) {
        serial_write("[acpi_power] no RSDP\n");
        return false;
    }

    uint32_t rsdt_addr = *((uint32_t *)(rsdp_ptr + 16));
    if (!rsdt_addr) return false;

    map_page_if_needed(rsdt_addr);
    acpi_hdr_t *rsdt = (acpi_hdr_t *)rsdt_addr;
    for (uint32_t off = 0; off < rsdt->length; off += 0x1000)
        map_page_if_needed(rsdt_addr + off);

    if (memcmp(rsdt->sig, "RSDT", 4) != 0) return false;

    /* Buscar FACP (FADT) en el RSDT */
    uint32_t n_entries = (rsdt->length - sizeof(acpi_hdr_t)) / 4;
    uint32_t *entries  = (uint32_t *)(rsdt_addr + sizeof(acpi_hdr_t));
    fadt_t   *fadt     = NULL;

    for (uint32_t i = 0; i < n_entries; i++) {
        if (!entries[i]) continue;
        map_page_if_needed(entries[i]);
        acpi_hdr_t *h = (acpi_hdr_t *)entries[i];
        if (memcmp(h->sig, "FACP", 4) == 0) {
            fadt = (fadt_t *)h;
            break;
        }
    }
    if (!fadt) { serial_write("[acpi_power] no FADT\n"); return false; }

    pm1a_cnt = (uint16_t)fadt->pm1a_cnt_blk;
    pm1b_cnt = (uint16_t)fadt->pm1b_cnt_blk;

    /* Reset register (FADT Rev ≥ 2) */
    if (fadt->hdr.rev >= 2 && fadt->reset_reg[0] == 1 /* SystemIO */) {
        uint64_t raddr;
        memcpy(&raddr, fadt->reset_reg + 4, 8);
        if ((raddr >> 32) == 0) {
            reset_reg_space  = 1;
            reset_reg_addr   = (uint32_t)raddr;
            reset_reg_value  = fadt->reset_value;
            have_reset_reg   = true;
        }
    }

    /* Buscar _S5_ en el DSDT */
    find_s5_in_dsdt(fadt->dsdt_addr);

    acpi_ok = true;
    kprintf("  [ OK ] ACPI power: PM1a=0x%x, reset=%s\n",
            pm1a_cnt, have_reset_reg ? "ACPI" : "KBC");
    return true;
}

void acpi_shutdown(void)
{
    if (acpi_ok && pm1a_cnt) {
        /* Escribir SLP_TYP + SLP_EN (bit 13) en PM1a_CNT_BLK */
        uint16_t val = (uint16_t)(slp_typa | (1u << 13));
        outw(pm1a_cnt, val);
        if (pm1b_cnt) outw(pm1b_cnt, (uint16_t)(slp_typb | (1u << 13)));
        sleep(500);
    }

    /* Fallback 1: QEMU old port */
    outw(0x604, 0x2000);
    sleep(100);
    /* Fallback 2: VirtualBox / Bochs */
    outw(0xB004, 0x2000);
    sleep(100);
    /* Fallback 3: ACPI S5 sin parseo de DSDT (tipo S5 = 5) */
    if (pm1a_cnt) outw(pm1a_cnt, (5 << 10) | (1u << 13));
    /* Si llegamos aquí, simplemente halt */
    __asm__ volatile("cli; hlt");
    for (;;) __asm__ volatile("hlt");
}

void acpi_reboot(void)
{
    /* Método 1: ACPI Reset Register */
    if (have_reset_reg) {
        outb((uint16_t)reset_reg_addr, reset_reg_value);
        sleep(100);
    }
    /* Método 2: Keyboard Controller (funciona en la mayoría de hardware pre-2010) */
    /* Esperar a que el KBC esté listo */
    for (int i = 0; i < 0x10000; i++) {
        if (!(inb(0x64) & 2)) break;
    }
    outb(0x64, 0xFE);   /* Pulse Reset line */
    sleep(100);

    /* Método 3: Triple Fault — siempre funciona */
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) bad_idt = {0, 0};
    __asm__ volatile(
        "lidt %0\n"
        "int $0\n"
        : : "m"(bad_idt)
    );
    for (;;) __asm__ volatile("hlt");
}
