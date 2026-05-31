#ifndef DRIVERS_ATA_H
#define DRIVERS_ATA_H

#include "../lib/types.h"

/* Sector size in bytes for ATA/IDE devices. */
#define ATA_SECTOR_SIZE 512

/* Probe the primary ATA bus (master). Returns true if a disk is present.
 * Must be called once at boot before any read/write. */
bool ata_init(void);

/* True if a usable disk was detected by ata_init(). */
bool ata_present(void);

/* Total number of addressable sectors reported by the drive (LBA28). */
uint32_t ata_total_sectors(void);

/* Read `count` sectors starting at LBA `lba` into `buf`
 * (buf must hold count*512 bytes). Returns 0 on success, <0 on error. */
int ata_read_sectors(uint32_t lba, uint8_t count, void *buf);

/* Write `count` sectors starting at LBA `lba` from `buf`.
 * Returns 0 on success, <0 on error. */
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);

#endif /* DRIVERS_ATA_H */
