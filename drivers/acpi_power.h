/* drivers/acpi_power.h — ACPI power management real para Trinux.
 *
 * CAMBIO #8: power-off y reset vía ACPI, funciona en hardware real moderno.
 *
 * El método anterior usaba el puerto 0x604 (QEMU-only). En hardware real
 * moderno (post-2000) se requiere ACPI para apagar limpiamente.
 *
 * Implementamos:
 *   - Parseo del DSDT/FACP para encontrar PM1a_CNT_BLK y SLP_TYPa
 *   - acpi_shutdown()  — S5 (soft off): escribe SLP_EN + SLP_TYP en PM1 CNT
 *   - acpi_reboot()    — reset via ACPI Reset Register (FADT Rev 2+)
 *                        con fallback a keyboard controller (0x64) y triple fault
 *
 * Si ACPI no está disponible, cae al método legacy (outw(0x604, 0x2000) para
 * QEMU o teclado controller para hardware antiguo).
 */
#ifndef DRIVERS_ACPI_POWER_H
#define DRIVERS_ACPI_POWER_H

#include "../lib/types.h"

/* Inicializa el subsistema ACPI power (parsea FACP/FADT).
 * Devuelve true si encontró las estructuras ACPI necesarias. */
bool acpi_power_init(void);

/* Apaga el sistema (S5 / soft-off).
 * Si ACPI funciona: escribe SLP_EN en PM1a_CNT_BLK.
 * Fallback: outw(0x604, 0x2000) [QEMU] o outw(0xB004, 0x2000) [Bochs/VBox].
 * No regresa si el apagado tiene éxito. */
void acpi_shutdown(void);

/* Reinicia el sistema.
 * Orden de intentos:
 *   1. ACPI Reset Register (FADT rev ≥ 2, hardware moderno)
 *   2. Keyboard controller (0x64 write 0xFE — hardware antiguo)
 *   3. Triple fault (siempre funciona como último recurso)
 * No regresa. */
void acpi_reboot(void) __attribute__((noreturn));

#endif /* DRIVERS_ACPI_POWER_H */
