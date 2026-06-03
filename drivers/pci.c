/* drivers/pci.c  -  PCI configuration space access (mechanism 1).
 *
 * Uses I/O ports 0xCF8 (address) and 0xCFC (data) to read/write the 256-byte
 * configuration space of any PCI device.  This is the standard x86 method and
 * works on every PC since the mid-90s.
 */
#include "pci.h"
#include "../cpu/ports.h"
#include "../lib/printf.h"
#include "../drivers/serial.h"

/* Build the 32-bit address for PCI config access.
 * Bit 31 = enable, bits 23:16 = bus, 15:11 = device, 10:8 = function,
 * 7:2 = register (dword-aligned), 1:0 = 0. */
static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return (uint32_t)(
        (1u << 31) |
        ((uint32_t)bus  << 16) |
        ((uint32_t)(slot & 0x1F) << 11) |
        ((uint32_t)(func & 0x07) << 8) |
        ((uint32_t)(off & 0xFC))
    );
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                 uint32_t value)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t dw = pci_read32(bus, slot, func, offset & ~3u);
    return (uint16_t)(dw >> ((offset & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t dw = pci_read32(bus, slot, func, offset & ~3u);
    return (uint8_t)(dw >> ((offset & 3) * 8));
}

/* Enable bus mastering (bit 2 of the Command register at offset 0x04).
 * AHCI needs this for DMA transfers. */
void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t cmd = pci_read16(bus, slot, func, 0x04);
    if (!(cmd & (1 << 2))) {
        cmd |= (1 << 2) | (1 << 1);   /* bus master + memory space */
        pci_write32(bus, slot, func, 0x04,
                    (pci_read32(bus, slot, func, 0x04) & 0xFFFF0000u) | cmd);
    }
}

/* Scan all 256 buses × 32 devices × 8 functions to find the first device
 * with class 0x01 (storage), subclass 0x06 (SATA), progIF 0x01 (AHCI 1.0).
 * Returns true and fills the output parameters if found. */
bool pci_find_ahci(uint8_t *o_bus, uint8_t *o_slot, uint8_t *o_func,
                   uint32_t *o_abar)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, slot, func, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0xFFFF || vendor == 0x0000)
                    continue;   /* no device */

                uint32_t classrev = pci_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t cls     = (uint8_t)(classrev >> 24);
                uint8_t subcls  = (uint8_t)(classrev >> 16);
                uint8_t progif  = (uint8_t)(classrev >> 8);

                if (cls == PCI_CLASS_STORAGE && subcls == PCI_SUBCLASS_SATA &&
                    progif == PCI_PROGIF_AHCI) {
                    /* BAR5 (offset 0x24) holds the AHCI Base Address (ABAR). */
                    uint32_t bar5 = pci_read32((uint8_t)bus, slot, func, 0x24);
                    bar5 &= ~0xFu;   /* mask type bits */

                    serial_printf("[pci] AHCI found at %u:%u.%u  "
                                 "vendor=%04x device=%04x  ABAR=%08x\n",
                                 bus, slot, func,
                                 vendor, (uint16_t)(id >> 16), bar5);

                    *o_bus  = (uint8_t)bus;
                    *o_slot = slot;
                    *o_func = func;
                    *o_abar = bar5;
                    return true;
                }

                /* If function 0 is not multifunction, skip funcs 1-7. */
                if (func == 0) {
                    uint8_t hdr = pci_read8((uint8_t)bus, slot, 0, 0x0E);
                    if (!(hdr & 0x80))
                        break;
                }
            }
        }
    }
    return false;
}
