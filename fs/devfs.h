/* fs/devfs.h  -  populate /dev with the classic Unix character devices.
 *
 * Creates VFS device nodes under /dev:
 *   /dev/sda     - raw access to the primary ATA master (only if a disk is
 *                  present). Reads/writes go byte-addressed through the ATA
 *                  driver (sector-aligned chunks are buffered transparently).
 *   /dev/zero    - infinite source of 0x00 bytes; writes are discarded.
 *   /dev/null    - the bit bucket: reads return EOF, writes are discarded.
 *   /dev/random  - pseudo-random byte stream (deterministic LCG seeded from
 *                  the timer; perfectly fine for educational use, not crypto).
 *
 * Call devfs_init() once at boot, AFTER ramfs_init() and ata_init().
 */
#ifndef FS_DEVFS_H
#define FS_DEVFS_H

#include "../lib/types.h"

void devfs_init(void);

#endif /* FS_DEVFS_H */
