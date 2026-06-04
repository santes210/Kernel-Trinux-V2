/* drivers/acpi_ec.c  -  ACPI Embedded Controller battery status reader.
 *
 * The Embedded Controller (EC) on laptops manages battery, thermals, fan,
 * keyboard backlight, etc.  It is accessed via I/O ports 0x62 (data) and
 * 0x66 (command/status), using the protocol defined in ACPI spec §12.2:
 *
 *   To read EC register N:
 *     1. Wait for IBF=0 on port 0x66
 *     2. Write 0x80 (READ_EC) to port 0x66
 *     3. Wait for IBF=0
 *     4. Write N to port 0x62
 *     5. Wait for OBF=1
 *     6. Read result from port 0x62
 *
 * The REGISTER ADDRESSES vary by laptop manufacturer.  This driver tries
 * multiple known layouts (HP, Lenovo, Dell, generic) and uses whichever
 * returns plausible values.  For HP Stream 14 (Bay Trail), the EC typically
 * stores battery percentage directly in a register around 0xA4-0xB0.
 *
 * If the EC doesn't respond (desktop PC, VM without EC), the driver
 * gracefully returns "no battery".
 */
#include "acpi_ec.h"
#include "../cpu/ports.h"
#include "../drivers/serial.h"
#include "../lib/string.h"

#define EC_DATA   0x62
#define EC_SC     0x66   /* status (read) / command (write) */

/* EC status register bits */
#define EC_OBF    0x01   /* Output Buffer Full — data ready to read */
#define EC_IBF    0x02   /* Input Buffer Full — EC busy, don't write */

/* EC commands */
#define EC_READ   0x80
#define EC_WRITE  0x81
#define EC_QUERY  0x84

static bool ec_detected;

/* Wait for IBF to clear (EC ready to accept input). */
static bool ec_wait_ibf_clear(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(EC_SC) & EC_IBF))
            return true;
    }
    return false;
}

/* Wait for OBF to set (EC has data for us). */
static bool ec_wait_obf_set(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(EC_SC) & EC_OBF)
            return true;
    }
    return false;
}

/* Read one EC register. Returns the byte, or -1 on timeout. */
static int ec_read_reg(uint8_t reg)
{
    if (!ec_wait_ibf_clear()) return -1;
    outb(EC_SC, EC_READ);
    if (!ec_wait_ibf_clear()) return -1;
    outb(EC_DATA, reg);
    if (!ec_wait_obf_set()) return -1;
    return (int)inb(EC_DATA);
}

/* Probe if the EC responds at all. */
bool acpi_ec_available(void)
{
    if (ec_detected) return true;

    /* Try reading status port — on machines without an EC, the port
     * typically returns 0xFF. */
    uint8_t status = inb(EC_SC);
    if (status == 0xFF) return false;

    /* Try reading a register. If the EC doesn't respond (timeout), bail. */
    int val = ec_read_reg(0x00);
    if (val < 0) return false;

    ec_detected = true;
    return true;
}

/* ---- Known EC register layouts for battery ----
 *
 * These are empirical: different laptop vendors put battery data at
 * different EC offsets.  We try them in order and pick the first one
 * that returns a plausible percentage (1-100).
 *
 * Format: { percent_reg, status_reg, voltage_lo, voltage_hi,
 *           remain_lo, remain_hi, full_lo, full_hi, rate_lo, rate_hi }
 * Use 0xFF for "not available". */

typedef struct {
    uint8_t pct;          /* percentage (0-100) */
    uint8_t sts;          /* status flags byte */
    uint8_t volt_lo, volt_hi;
    uint8_t rem_lo, rem_hi;
    uint8_t full_lo, full_hi;
    uint8_t rate_lo, rate_hi;
    const char *name;
} ec_layout_t;

static const ec_layout_t layouts[] = {
    /* HP Stream 14 (Bay Trail/Cherry Trail) — confirmed by EC dump */
    { 0x92, 0x40, 0x58, 0x59, 0x04, 0x05, 0xA8, 0xA9, 0xFF, 0xFF, "HP-Stream" },
    /* HP laptops common layout A */
    { 0xA4, 0xA0, 0xA6, 0xA7, 0xA2, 0xA3, 0xA8, 0xA9, 0xAA, 0xAB, "HP-A" },
    /* HP alternate layout B */
    { 0xB0, 0xA0, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, "HP-B" },
    /* Lenovo ThinkPad */
    { 0xB9, 0x38, 0xAA, 0xAB, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, "Lenovo" },
    /* Dell */
    { 0xA6, 0xA5, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, "Dell" },
    /* Generic: try common percentage-only registers */
    { 0x2C, 0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, "generic-2C" },
    { 0xB0, 0xB1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, "generic-B0" },
};
#define N_LAYOUTS ((int)(sizeof(layouts) / sizeof(layouts[0])))

static int best_layout = -1;

int acpi_ec_read_register(uint8_t reg)
{
    if (!acpi_ec_available()) return -1;
    return ec_read_reg(reg);
}

void acpi_ec_reset_layout(void)
{
    best_layout = -1;
}

/* Read a 16-bit value from two EC registers.
 * HP Stream stores values big-endian (first reg = high byte). */
static uint16_t read16_be(uint8_t hi_reg, uint8_t lo_reg)
{
    if (hi_reg == 0xFF || lo_reg == 0xFF) return 0;
    int h = ec_read_reg(hi_reg);
    int l = ec_read_reg(lo_reg);
    if (l < 0 || h < 0) return 0;
    return (uint16_t)((h << 8) | l);
}

bool acpi_ec_read_battery(battery_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->percentage = 0xFF;

    if (!acpi_ec_available())
        return false;

    /* If we haven't found a working layout yet, try all of them. */
    if (best_layout < 0) {
        for (int i = 0; i < N_LAYOUTS; i++) {
            int pct = ec_read_reg(layouts[i].pct);
            if (pct >= 0 && pct <= 100) {
                best_layout = i;
                serial_printf("[ec] using layout '%s' (pct reg 0x%02x = %d%%)\n",
                              layouts[i].name, layouts[i].pct, pct);
                break;
            }
        }
        if (best_layout < 0) {
            /* No layout produced a valid percentage.  As a last resort,
             * scan a range of EC registers for any value 1-100 that looks
             * like a percentage. */
            for (uint8_t r = 0x20; r <= 0xC0; r++) {
                int v = ec_read_reg(r);
                if (v >= 1 && v <= 100) {
                    serial_printf("[ec] auto-detect: reg 0x%02x = %d (using as %%)\n",
                                  r, v);
                    /* Create a minimal layout at runtime is hard with const,
                     * so we just read this register directly below. */
                    info->present = true;
                    info->percentage = (uint8_t)v;
                    /* Try to guess AC/charging from the same area */
                    int s = ec_read_reg(r > 0x20 ? r - 1 : r + 1);
                    if (s >= 0) {
                        info->ac_connected = (s & 0x10) != 0;
                        info->charging     = (s & 0x20) != 0 || (s & 0x02) != 0;
                        info->discharging  = (s & 0x40) != 0 || (s & 0x01) != 0;
                    }
                    return true;
                }
            }
            serial_write("[ec] no valid battery layout found\n");
            return false;
        }
    }

    const ec_layout_t *lay = &layouts[best_layout];

    int pct = ec_read_reg(lay->pct);
    if (pct < 0 || pct > 100) return false;

    info->present    = true;
    info->percentage = (uint8_t)pct;

    int sts = ec_read_reg(lay->sts);
    if (sts >= 0) {
        if (best_layout == 0) {
            /* HP Stream 14: reg 0x40 — bit0=AC, bit1=discharging,
             * bit2=? . Confirmed by EC dump: 0x02 = discharging on bat. */
            info->ac_connected = (sts & 0x01) != 0;
            info->discharging  = (sts & 0x02) != 0;
            info->charging     = info->ac_connected && !info->discharging;
        } else {
            /* Generic bit patterns for other vendors: */
            info->ac_connected = (sts & 0x10) != 0;
            info->charging     = (sts & 0x02) != 0;
            info->discharging  = (sts & 0x01) != 0;
            if (!info->charging && !info->discharging && info->ac_connected)
                info->charging = (pct < 100);
        }
    }

    info->voltage_mv = read16_be(lay->volt_lo, lay->volt_hi);
    info->remain_mah = read16_be(lay->rem_lo, lay->rem_hi);
    info->full_mah   = read16_be(lay->full_lo, lay->full_hi);
    info->rate_ma    = read16_be(lay->rate_lo, lay->rate_hi);

    return true;
}
