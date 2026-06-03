#ifndef FS_DISKFS_H
#define FS_DISKFS_H

#include "../lib/types.h"

/* Persistence layer: serialize the in-RAM VFS tree to the ATA disk and
 * restore it at boot, so files survive a reboot/power-off.
 *
 * On-disk layout (little-endian), starting at LBA 0:
 *   [superblock | node 0 | node 1 | ... ]
 * The whole image is a byte stream padded up to a sector boundary; we write
 * it with the ATA driver one sector (512 B) at a time.
 */

/* Try to load a saved filesystem image from disk into the live ramfs tree.
 * Returns:
 *    1  loaded successfully (existing image found)
 *    0  no valid image on disk (fresh disk -> keep default tree)
 *   <0  disk/format error
 * Call this AFTER ramfs_init() so a valid image replaces the default tree. */
int diskfs_load(void);

/* Serialize the current VFS tree and write it to disk.
 * Returns 0 on success, <0 on error (no disk, too big, write failure). */
int diskfs_save(void);

/* True if a usable disk is present (mirrors ata_present). */
bool diskfs_available(void);

/* Total size of the attached disk in bytes (0 if no disk). */
uint32_t diskfs_total_bytes(void);    /* NOTE: wraps above 4 GiB; use _mb() */
uint32_t diskfs_total_mb(void);       /* total disk size in MiB (no overflow) */

/* Size in bytes that the CURRENT in-RAM filesystem tree would occupy on disk
 * (superblock + serialized nodes). Reflects live files, even before `sync`. */
uint32_t diskfs_used_bytes(void);

#endif /* FS_DISKFS_H */
