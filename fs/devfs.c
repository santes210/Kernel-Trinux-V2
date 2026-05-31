/* fs/devfs.c  -  /dev/{sda,zero,null,random} character devices.
 *
 * Each device is a regular VFS node of type VFS_DEVICE with its own read/write
 * callbacks. This means every existing tool (cat, hexdump, dd, cp, ...) works
 * with them out of the box, because they all go through vfs_read/vfs_write.
 *
 * /dev/sda is the most interesting one: it exposes the raw disk as a byte
 * stream. Since ATA only knows about 512-byte sectors, we transparently
 * read-modify-write whole sectors when the caller's range is not aligned.
 * `dd if=/dev/sda of=backup.img bs=512 count=100` Just Works.
 */
#include "devfs.h"
#include "vfs.h"
#include "ramfs.h"
#include "../drivers/ata.h"
#include "../drivers/timer.h"
#include "../lib/string.h"

/* ---------- /dev/zero ---------- */

static uint32_t zero_read(vfs_node_t *n, uint32_t off, uint32_t size,
                          uint8_t *buf)
{
    (void)n; (void)off;
    memset(buf, 0, size);
    return size;
}

static uint32_t zero_write(vfs_node_t *n, uint32_t off, uint32_t size,
                           uint8_t *buf)
{
    (void)n; (void)off; (void)buf;
    return size;   /* silently consume everything */
}

/* ---------- /dev/null ---------- */

static uint32_t null_read(vfs_node_t *n, uint32_t off, uint32_t size,
                          uint8_t *buf)
{
    (void)n; (void)off; (void)size; (void)buf;
    return 0;      /* always EOF */
}

static uint32_t null_write(vfs_node_t *n, uint32_t off, uint32_t size,
                           uint8_t *buf)
{
    (void)n; (void)off; (void)buf;
    return size;
}

/* ---------- /dev/random  (LCG, NOT cryptographic) ---------- */

static uint32_t rand_state = 0xCAFEBABEu;

static uint32_t rand_next(void)
{
    /* Numerical Recipes LCG; perfectly fine for an educational kernel. */
    rand_state = rand_state * 1664525u + 1013904223u;
    return rand_state;
}

static uint32_t random_read(vfs_node_t *n, uint32_t off, uint32_t size,
                            uint8_t *buf)
{
    (void)n; (void)off;
    /* Stir with the current tick count so consecutive opens differ. */
    rand_state ^= uptime() * 2654435761u;
    for (uint32_t i = 0; i < size; i++)
        buf[i] = (uint8_t)(rand_next() >> 24);
    return size;
}

static uint32_t random_write(vfs_node_t *n, uint32_t off, uint32_t size,
                             uint8_t *buf)
{
    /* Writes mix into the entropy pool. */
    (void)n; (void)off;
    for (uint32_t i = 0; i < size; i++)
        rand_state = rand_state * 31u + buf[i];
    return size;
}

/* ---------- /dev/sda  (raw ATA disk, byte-addressed) ---------- */

/* Read `size` bytes starting at byte offset `off` from the raw disk into buf.
 * Handles unaligned starts/ends by reading the surrounding sector(s). */
static uint32_t sda_read(vfs_node_t *n, uint32_t off, uint32_t size,
                         uint8_t *buf)
{
    (void)n;
    if (!ata_present()) return 0;

    uint32_t disk_bytes = ata_total_sectors() * ATA_SECTOR_SIZE;
    if (off >= disk_bytes) return 0;
    if (off + size > disk_bytes) size = disk_bytes - off;
    if (size == 0) return 0;

    uint8_t sect[ATA_SECTOR_SIZE];
    uint32_t done = 0;
    while (done < size) {
        uint32_t pos     = off + done;
        uint32_t lba     = pos / ATA_SECTOR_SIZE;
        uint32_t soff    = pos % ATA_SECTOR_SIZE;
        uint32_t want    = size - done;

        /* Fast path: aligned full-sector reads, do them in big chunks. */
        if (soff == 0 && want >= ATA_SECTOR_SIZE) {
            uint32_t nsec = want / ATA_SECTOR_SIZE;
            if (nsec > 255) nsec = 255;     /* LBA28 PIO limit per command */
            if (ata_read_sectors(lba, (uint8_t)nsec, buf + done) != 0)
                break;
            done += nsec * ATA_SECTOR_SIZE;
            continue;
        }

        /* Slow path: partial sector, buffer one sector and memcpy. */
        if (ata_read_sectors(lba, 1, sect) != 0) break;
        uint32_t chunk = ATA_SECTOR_SIZE - soff;
        if (chunk > want) chunk = want;
        memcpy(buf + done, sect + soff, chunk);
        done += chunk;
    }
    return done;
}

/* Write `size` bytes to the raw disk at byte offset `off`. Unaligned regions
 * trigger a read-modify-write of the affected sector(s). */
static uint32_t sda_write(vfs_node_t *n, uint32_t off, uint32_t size,
                          uint8_t *buf)
{
    (void)n;
    if (!ata_present()) return 0;

    uint32_t disk_bytes = ata_total_sectors() * ATA_SECTOR_SIZE;
    if (off >= disk_bytes) return 0;
    if (off + size > disk_bytes) size = disk_bytes - off;
    if (size == 0) return 0;

    uint8_t sect[ATA_SECTOR_SIZE];
    uint32_t done = 0;
    while (done < size) {
        uint32_t pos  = off + done;
        uint32_t lba  = pos / ATA_SECTOR_SIZE;
        uint32_t soff = pos % ATA_SECTOR_SIZE;
        uint32_t want = size - done;

        if (soff == 0 && want >= ATA_SECTOR_SIZE) {
            uint32_t nsec = want / ATA_SECTOR_SIZE;
            if (nsec > 255) nsec = 255;
            if (ata_write_sectors(lba, (uint8_t)nsec, buf + done) != 0)
                break;
            done += nsec * ATA_SECTOR_SIZE;
            continue;
        }

        /* read-modify-write the partial sector */
        if (ata_read_sectors(lba, 1, sect) != 0) break;
        uint32_t chunk = ATA_SECTOR_SIZE - soff;
        if (chunk > want) chunk = want;
        memcpy(sect + soff, buf + done, chunk);
        if (ata_write_sectors(lba, 1, sect) != 0) break;
        done += chunk;
    }
    return done;
}

/* ---------- init ---------- */

static vfs_node_t *make_dev(vfs_node_t *dev_dir, const char *name,
                            read_fn r, write_fn w, uint32_t size,
                            uint32_t perms)
{
    vfs_node_t *n = ramfs_create_node(dev_dir, name, VFS_DEVICE);
    if (!n) return NULL;
    n->read        = r;
    n->write       = w;
    n->size        = size;
    n->permissions = perms;
    n->owner_uid   = 0;   /* root */
    n->owner_gid   = 0;
    return n;
}

void devfs_init(void)
{
    /* /dev was created by ramfs_init(); just look it up. */
    vfs_node_t *root = vfs_get_root();
    vfs_node_t *dev  = vfs_finddir(root, "dev");
    if (!dev) return;

    /* world-readable pseudo devices */
    make_dev(dev, "zero",   zero_read,   zero_write,   0, 0666);
    make_dev(dev, "null",   null_read,   null_write,   0, 0666);
    make_dev(dev, "random", random_read, random_write, 0, 0644);

    /* /dev/sda only if a real disk is attached. Its "size" is the disk size
     * in bytes, so `stat /dev/sda` and `ls -l /dev/sda` show the capacity. */
    if (ata_present()) {
        uint32_t bytes = ata_total_sectors() * ATA_SECTOR_SIZE;
        /* root rw, group/others read-only — same as Linux's default for sdX */
        make_dev(dev, "sda", sda_read, sda_write, bytes, 0640);
    }
}
