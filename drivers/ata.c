/* drivers/ata.c  -  Unified disk driver: tries legacy IDE first, then AHCI.
 *
 * The rest of the kernel (diskfs, blockfs, devfs, etc.) calls the ata_*()
 * API exclusively.  This file tries IDE PIO on ports 0x1F0 first (works in
 * QEMU and on old hardware).  If that fails, it probes PCI for an AHCI
 * controller (works on all modern PCs, including USB-booted laptops where
 * the chipset exposes the boot medium via SATA/AHCI).
 *
 * The public API is unchanged: ata_init / ata_present / ata_total_sectors /
 * ata_read_sectors / ata_write_sectors.  Callers never need to know which
 * backend is active.
 */
#include "ata.h"
#include "ahci.h"
#include "../cpu/ports.h"
#include "../drivers/serial.h"

/* ====================================================================
 *  Legacy IDE PIO (primary bus, master drive, LBA28)
 * ==================================================================== */

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_CONTROL     0x3F6

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_RDY  0x40
#define ST_BSY  0x80

#define CMD_READ_PIO    0x20
#define CMD_WRITE_PIO   0x30
#define CMD_CACHE_FLUSH 0xE7
#define CMD_IDENTIFY    0xEC

/* Which backend is active? */
enum disk_backend { DISK_NONE, DISK_IDE, DISK_AHCI };

static enum disk_backend backend;
static uint32_t          total_sectors;

/* ---- IDE helpers ---- */

static void insw_buf(uint16_t port, void *buf, uint32_t words)
{
    uint16_t *p = (uint16_t *)buf;
    for (uint32_t i = 0; i < words; i++)
        p[i] = inw(port);
}

static void outsw_buf(uint16_t port, const void *buf, uint32_t words)
{
    const uint16_t *p = (const uint16_t *)buf;
    for (uint32_t i = 0; i < words; i++)
        outw(port, p[i]);
}

static void ata_400ns(void)
{
    for (int i = 0; i < 4; i++)
        (void)inb(ATA_CONTROL);
}

static int ata_poll(void)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return -1;
        if (s & ST_DRQ) return 0;
    }
    return -2;
}

static int ata_wait_ready(void)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & ST_BSY))
            return (s & (ST_ERR | ST_DF)) ? -1 : 0;
    }
    return -2;
}

static bool ide_init(void)
{
    outb(ATA_DRIVE, 0xE0);
    ata_400ns();
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, CMD_IDENTIFY);
    ata_400ns();

    uint8_t status = inb(ATA_STATUS);
    if (status == 0) return false;

    for (int i = 0; i < 100000; i++) {
        status = inb(ATA_STATUS);
        if (!(status & ST_BSY)) break;
    }
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0)
        return false;

    for (int i = 0; i < 100000; i++) {
        status = inb(ATA_STATUS);
        if (status & ST_ERR) return false;
        if (status & ST_DRQ) break;
    }
    if (!(status & ST_DRQ)) return false;

    uint16_t id[256];
    insw_buf(ATA_DATA, id, 256);
    total_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    return true;
}

static int ide_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    uint32_t n = (count == 0) ? 256 : count;

    for (uint32_t s = 0; s < n; s++) {
        uint32_t cur = lba + s;
        outb(ATA_DRIVE, 0xE0 | ((cur >> 24) & 0x0F));
        outb(ATA_ERROR, 0);
        outb(ATA_SECCOUNT, 1);
        outb(ATA_LBA_LO,  (uint8_t)(cur & 0xFF));
        outb(ATA_LBA_MID, (uint8_t)((cur >> 8) & 0xFF));
        outb(ATA_LBA_HI,  (uint8_t)((cur >> 16) & 0xFF));
        outb(ATA_COMMAND, CMD_READ_PIO);
        if (ata_poll() != 0) return -1;
        insw_buf(ATA_DATA, out + done, ATA_SECTOR_SIZE / 2);
        done += ATA_SECTOR_SIZE;
        ata_400ns();
    }
    return 0;
}

static int ide_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;
    uint32_t n = (count == 0) ? 256 : count;

    for (uint32_t s = 0; s < n; s++) {
        uint32_t cur = lba + s;
        outb(ATA_DRIVE, 0xE0 | ((cur >> 24) & 0x0F));
        outb(ATA_ERROR, 0);
        outb(ATA_SECCOUNT, 1);
        outb(ATA_LBA_LO,  (uint8_t)(cur & 0xFF));
        outb(ATA_LBA_MID, (uint8_t)((cur >> 8) & 0xFF));
        outb(ATA_LBA_HI,  (uint8_t)((cur >> 16) & 0xFF));
        outb(ATA_COMMAND, CMD_WRITE_PIO);
        if (ata_poll() != 0) return -1;
        outsw_buf(ATA_DATA, in + done, ATA_SECTOR_SIZE / 2);
        done += ATA_SECTOR_SIZE;
        outb(ATA_COMMAND, CMD_CACHE_FLUSH);
        if (ata_wait_ready() != 0) return -2;
        ata_400ns();
    }
    return 0;
}

/* ====================================================================
 *  Public API (delegates to whichever backend is active)
 * ==================================================================== */

bool ata_init(void)
{
    backend = DISK_NONE;
    total_sectors = 0;

    /* Try legacy IDE first (fast, no PCI scan needed). */
    if (ide_init()) {
        backend = DISK_IDE;
        serial_write("[ata] legacy IDE disk found\n");
        return true;
    }

    /* No IDE — try AHCI (covers modern laptops, USB-booted PCs, etc.). */
    serial_write("[ata] no legacy IDE; probing PCI for AHCI...\n");
    if (ahci_init()) {
        backend = DISK_AHCI;
        total_sectors = ahci_total_sectors();
        serial_write("[ata] using AHCI backend\n");
        return true;
    }

    serial_write("[ata] no disk found (neither IDE nor AHCI)\n");
    return false;
}

bool ata_present(void)
{
    return backend != DISK_NONE;
}

uint32_t ata_total_sectors(void)
{
    return total_sectors;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    switch (backend) {
    case DISK_IDE:
        return ide_read_sectors(lba, count, buf);
    case DISK_AHCI:
        return ahci_read_sectors(lba, (uint16_t)(count == 0 ? 256 : count), buf);
    default:
        return -10;
    }
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
    switch (backend) {
    case DISK_IDE:
        return ide_write_sectors(lba, count, buf);
    case DISK_AHCI:
        return ahci_write_sectors(lba, (uint16_t)(count == 0 ? 256 : count), buf);
    default:
        return -10;
    }
}
