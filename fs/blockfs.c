/* fs/blockfs.c  -  4 KiB block allocator for disk-backed file contents.
 *
 * Layout on the ATA disk (512-byte sectors):
 *
 *   sector 0 ............ BOOT_RESERVE_SECTORS-1   bootloader / kernel image
 *   BITMAP_LBA ......... bitmap region (BITMAP_SECTORS sectors)
 *   DATA_START_LBA ..... 4 KiB data blocks (grows toward the diskfs tail)
 *   (disk tail) ........ diskfs metadata snapshot (fs/diskfs.c, last 64 MiB)
 *
 * We keep the bitmap in RAM while running and flush it to disk on sync. The
 * data area spans everything between DATA_START_LBA and the diskfs tail.
 */
#include "blockfs.h"
#include "../drivers/ata.h"
#include "../drivers/serial.h"
#include "../mm/kheap.h"
#include "../lib/string.h"

/* Must match diskfs.c: the metadata snapshot occupies the last 64 MiB. */
#define DISKFS_TAIL_BYTES   (64u * 1024u * 1024u)
#define DISKFS_TAIL_SECTORS (DISKFS_TAIL_BYTES / 512u)

/* Reserve the first 16 MiB for the bootloader + kernel image. */
#define BOOT_RESERVE_SECTORS (16u * 1024u * 1024u / 512u)

/* Bitmap lives right after the boot reserve. */
#define BITMAP_LBA  BOOT_RESERVE_SECTORS

static bool      available;
static uint8_t  *bitmap;            /* RAM copy of the free/used bitmap */
static uint32_t  bitmap_bytes;      /* size of bitmap in bytes */
static uint32_t  bitmap_sectors;    /* sectors the bitmap occupies on disk */
static uint32_t  data_start_lba;    /* first sector of the data area */
static uint32_t  total_blocks;      /* number of 4 KiB data blocks */
static uint32_t  used_blocks;

static void bit_set(uint32_t i)   { bitmap[i >> 3] |=  (uint8_t)(1u << (i & 7)); }
static void bit_clear(uint32_t i) { bitmap[i >> 3] &= (uint8_t)~(1u << (i & 7)); }
static bool bit_test(uint32_t i)  { return (bitmap[i >> 3] >> (i & 7)) & 1u; }

bool blockfs_available(void) { return available; }
uint32_t blockfs_total_blocks(void) { return total_blocks; }
uint32_t blockfs_used_blocks(void)  { return used_blocks; }

bool blockfs_init(void)
{
    available = false;
    if (!ata_present())
        return false;

    uint32_t total_sectors = ata_total_sectors();
    /* Need room for: boot reserve + some bitmap + data + diskfs tail. */
    if (total_sectors <= BOOT_RESERVE_SECTORS + DISKFS_TAIL_SECTORS + 64)
        return false;   /* disk too small for block storage; RAM mode only */

    /* Sectors available between the boot reserve and the diskfs tail. */
    uint32_t avail_sectors =
        total_sectors - BOOT_RESERVE_SECTORS - DISKFS_TAIL_SECTORS;

    /* Estimate block count: each block is 8 sectors and needs 1 bitmap bit.
     * Reserve a slice for the bitmap first (1 bit per block => tiny). */
    uint32_t approx_blocks = avail_sectors / SECTORS_PER_BLOCK;
    bitmap_bytes   = (approx_blocks + 7) / 8;
    bitmap_sectors = (bitmap_bytes + 511) / 512;
    if (bitmap_sectors == 0) bitmap_sectors = 1;

    data_start_lba = BITMAP_LBA + bitmap_sectors;
    uint32_t data_sectors =
        total_sectors - data_start_lba - DISKFS_TAIL_SECTORS;
    total_blocks = data_sectors / SECTORS_PER_BLOCK;
    if (total_blocks == 0)
        return false;

    bitmap = (uint8_t *)kmalloc(bitmap_bytes);
    if (!bitmap)
        return false;

    /* Load the existing bitmap from disk; if it looks blank, start fresh. */
    uint8_t *secbuf = (uint8_t *)kmalloc(bitmap_sectors * 512);
    if (!secbuf) { kfree(bitmap); bitmap = NULL; return false; }

    bool ok = true;
    uint32_t done = 0;
    while (done < bitmap_sectors) {
        uint32_t chunk = bitmap_sectors - done;
        if (chunk > 255) chunk = 255;
        if (ata_read_sectors(BITMAP_LBA + done, (uint8_t)chunk,
                             secbuf + done * 512) != 0) { ok = false; break; }
        done += chunk;
    }
    if (ok)
        memcpy(bitmap, secbuf, bitmap_bytes);
    else
        memset(bitmap, 0, bitmap_bytes);
    kfree(secbuf);

    /* Recount used blocks from the loaded bitmap. */
    used_blocks = 0;
    for (uint32_t i = 0; i < total_blocks; i++)
        if (bit_test(i)) used_blocks++;

    available = true;
    serial_write("[blockfs] block storage ready\n");
    return true;
}

uint32_t blockfs_alloc(void)
{
    if (!available)
        return BLOCK_INVALID;
    for (uint32_t i = 0; i < total_blocks; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            used_blocks++;
            return i;
        }
    }
    return BLOCK_INVALID;   /* disk full */
}

void blockfs_free(uint32_t block)
{
    if (!available || block >= total_blocks)
        return;
    if (bit_test(block)) {
        bit_clear(block);
        if (used_blocks) used_blocks--;
    }
}

int blockfs_read_block(uint32_t block, void *buf)
{
    if (!available || block >= total_blocks)
        return -1;
    uint32_t lba = data_start_lba + block * SECTORS_PER_BLOCK;
    return ata_read_sectors(lba, SECTORS_PER_BLOCK, buf);
}

int blockfs_write_block(uint32_t block, const void *buf)
{
    if (!available || block >= total_blocks)
        return -1;
    uint32_t lba = data_start_lba + block * SECTORS_PER_BLOCK;
    return ata_write_sectors(lba, SECTORS_PER_BLOCK, buf);
}

int blockfs_flush_bitmap(void)
{
    if (!available)
        return -1;
    uint8_t *secbuf = (uint8_t *)kmalloc(bitmap_sectors * 512);
    if (!secbuf)
        return -2;
    memset(secbuf, 0, bitmap_sectors * 512);
    memcpy(secbuf, bitmap, bitmap_bytes);

    int rc = 0;
    uint32_t done = 0;
    while (done < bitmap_sectors) {
        uint32_t chunk = bitmap_sectors - done;
        if (chunk > 32) chunk = 32;   /* smaller chunks for USB compat */
        if (ata_write_sectors(BITMAP_LBA + done, (uint8_t)chunk,
                              secbuf + done * 512) != 0) { rc = -3; break; }
        done += chunk;
    }
    kfree(secbuf);
    return rc;
}
