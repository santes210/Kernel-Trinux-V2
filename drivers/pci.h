#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include "../lib/types.h"

/* PCI configuration space access (mechanism 1: ports 0xCF8/0xCFC). */

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* Read a 32-bit dword from PCI config space. */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/* Write a 32-bit dword to PCI config space. */
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                 uint32_t value);

/* Convenience readers. */
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/* PCI class codes we care about. */
#define PCI_CLASS_STORAGE       0x01
#define PCI_SUBCLASS_SATA       0x06
#define PCI_PROGIF_AHCI         0x01

/* Scan PCI and find the first AHCI controller.
 * Returns true if found, filling bus/slot/func/abar. */
bool pci_find_ahci(uint8_t *bus, uint8_t *slot, uint8_t *func,
                   uint32_t *abar);

/* Enable bus-mastering for a PCI device (required for AHCI DMA). */
void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func);

#endif /* DRIVERS_PCI_H */
