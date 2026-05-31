/* drivers/ata.c  -  Primary ATA/IDE disk driver (28-bit LBA, PIO mode).
 *
 * This is the classic, simple way to talk to a hard disk on x86: we poll the
 * status register (no IRQ) and move data through the 16-bit data port with
 * `insw`/`outsw`. It targets the PRIMARY bus, MASTER drive, which is what QEMU
 * exposes with `-drive file=disk.img,format=raw,if=ide`.
 *
 * I/O ports (primary bus):
 *   0x1F0  data (16-bit)        0x1F1  error/features
 *   0x1F2  sector count         0x1F3  LBA low
 *   0x1F4  LBA mid              0x1F5  LBA high
 *   0x1F6  drive/head (LBA bits 24-27 + drive select)
 *   0x1F7  status (read) / command (write)
 *   0x3F6  alt status / device control
 */
#include "ata.h"
#include "../cpu/ports.h"

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

/* Status register bits */
#define ST_ERR  0x01    /* error */
#define ST_DRQ  0x08    /* data request ready */
#define ST_SRV  0x10
#define ST_DF   0x20    /* drive fault */
#define ST_RDY  0x40    /* ready */
#define ST_BSY  0x80    /* busy */

/* Commands */
#define CMD_READ_PIO    0x20
#define CMD_WRITE_PIO   0x30
#define CMD_CACHE_FLUSH 0xE7
#define CMD_IDENTIFY    0xEC

static bool     have_disk;
static uint32_t total_sectors;

/* Read the 16-bit data port `words` times into a buffer. */
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

/* Small 400ns delay: read alt-status 4 times. */
static void ata_400ns(void)
{
    for (int i = 0; i < 4; i++)
        (void)inb(ATA_CONTROL);
}

/* Poll until BSY clears and DRQ sets (or error). Returns 0 ok, <0 error. */
static int ata_poll(void)
{
    /* wait while busy */
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_BSY)
            continue;
        if (s & (ST_ERR | ST_DF))
            return -1;
        if (s & ST_DRQ)
            return 0;
    }
    return -2;   /* timeout */
}

/* Wait for BSY to clear (used after non-data commands like cache flush). */
static int ata_wait_ready(void)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & ST_BSY))
            return (s & (ST_ERR | ST_DF)) ? -1 : 0;
    }
    return -2;
}

bool ata_init(void)
{
    have_disk = false;
    total_sectors = 0;

    /* Select master drive, LBA mode bit set. */
    outb(ATA_DRIVE, 0xE0);
    ata_400ns();

    /* Zero the params and issue IDENTIFY. */
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, CMD_IDENTIFY);
    ata_400ns();

    uint8_t status = inb(ATA_STATUS);
    if (status == 0)
        return false;          /* no drive present */

    /* Wait for BSY to clear. */
    for (int i = 0; i < 100000; i++) {
        status = inb(ATA_STATUS);
        if (!(status & ST_BSY))
            break;
    }

    /* Non-ATA (e.g. ATAPI) sets LBA_MID/HI non-zero -> not our simple disk. */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0)
        return false;

    /* Wait for DRQ or ERR. */
    for (int i = 0; i < 100000; i++) {
        status = inb(ATA_STATUS);
        if (status & ST_ERR)
            return false;
        if (status & ST_DRQ)
            break;
    }
    if (!(status & ST_DRQ))
        return false;

    /* Read the 256-word IDENTIFY block. */
    uint16_t id[256];
    insw_buf(ATA_DATA, id, 256);

    /* Words 60-61 hold the LBA28 total sector count. */
    total_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    have_disk = true;
    return true;
}

bool     ata_present(void)        { return have_disk; }
uint32_t ata_total_sectors(void)  { return total_sectors; }

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    if (!have_disk)
        return -10;

    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    /* count==0 means 256 sectors in ATA, but we read one chunk at a time. */
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

        if (ata_poll() != 0)
            return -1;
        insw_buf(ATA_DATA, out + done, ATA_SECTOR_SIZE / 2);
        done += ATA_SECTOR_SIZE;
        ata_400ns();
    }
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
    if (!have_disk)
        return -10;

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

        if (ata_poll() != 0)
            return -1;
        outsw_buf(ATA_DATA, in + done, ATA_SECTOR_SIZE / 2);
        done += ATA_SECTOR_SIZE;

        /* Flush the drive's write cache after each sector. */
        outb(ATA_COMMAND, CMD_CACHE_FLUSH);
        if (ata_wait_ready() != 0)
            return -2;
        ata_400ns();
    }
    return 0;
}
