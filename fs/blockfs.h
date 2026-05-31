#ifndef FS_BLOCKFS_H
#define FS_BLOCKFS_H

#include "../lib/types.h"

/* Block allocator for disk-backed file contents.
 *
 * The disk is divided into 4 KiB blocks. A bitmap (stored at a fixed disk
 * region) tracks which blocks are free. File contents live in these blocks,
 * read/written on demand, so file data no longer consumes kernel heap and the
 * total storage is bounded by the disk size, not by RAM.
 *
 * Disk layout (when block storage is active), using 512-byte sectors:
 *   [ 0 .. boot reserve )       : bootloader / kernel image area (untouched)
 *   [ data area ]               : 4 KiB data blocks (the bulk of the disk)
 *   [ bitmap region (tail-ish) ]: free/used bitmap for the data blocks
 *   [ diskfs metadata image    ]: the MKFS tree snapshot (fs/diskfs.c)
 *
 * blockfs places its data area + bitmap BEFORE the diskfs metadata tail so the
 * two never overlap.
 */

#define BLOCK_SIZE      4096u
#define SECTORS_PER_BLOCK (BLOCK_SIZE / 512u)   /* 8 */
#define BLOCK_INVALID   0xFFFFFFFFu

/* Initialise the allocator over the current ATA disk. Must be called after
 * ata_init(). Returns true if block storage is available. */
bool blockfs_init(void);

/* True if a disk + block storage is usable. */
bool blockfs_available(void);

/* Allocate one free 4 KiB block; returns its index or BLOCK_INVALID if full. */
uint32_t blockfs_alloc(void);

/* Free a previously allocated block. */
void blockfs_free(uint32_t block);

/* Read / write exactly one 4 KiB block (buf must be BLOCK_SIZE bytes). */
int blockfs_read_block(uint32_t block, void *buf);
int blockfs_write_block(uint32_t block, const void *buf);

/* Total / used data blocks (for df/top stats). */
uint32_t blockfs_total_blocks(void);
uint32_t blockfs_used_blocks(void);

/* Persist the allocation bitmap to disk (called by sync). */
int blockfs_flush_bitmap(void);

#endif /* FS_BLOCKFS_H */
