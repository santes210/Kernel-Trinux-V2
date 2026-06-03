#ifndef DRIVERS_ACPI_EC_H
#define DRIVERS_ACPI_EC_H

#include "../lib/types.h"

/* ACPI Embedded Controller (EC) interface for battery status.
 *
 * Standard ACPI EC ports: 0x62 (data), 0x66 (command/status).
 * Read/write EC registers using commands 0x80 (read) / 0x81 (write).
 *
 * Battery register layout varies by manufacturer, but we try several
 * common mappings and report whatever works. */

/* Battery info structure. */
typedef struct {
    bool     present;        /* battery detected */
    bool     charging;       /* AC connected and charging */
    bool     discharging;    /* running on battery */
    bool     ac_connected;   /* AC power present */
    uint8_t  percentage;     /* 0-100, or 0xFF if unknown */
    uint16_t voltage_mv;     /* millivolts, or 0 if unknown */
    uint16_t rate_ma;        /* current draw in mA, or 0 */
    uint16_t remain_mah;     /* remaining capacity mAh, or 0 */
    uint16_t full_mah;       /* full charge capacity mAh, or 0 */
} battery_info_t;

/* Try to detect and read battery status from the EC.
 * Returns true if a battery was detected. */
bool acpi_ec_read_battery(battery_info_t *info);

/* Check if EC is accessible at all. */
bool acpi_ec_available(void);

#endif /* DRIVERS_ACPI_EC_H */
