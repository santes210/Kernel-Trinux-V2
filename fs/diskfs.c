/* fs/diskfs.c  -  persist the in-RAM VFS tree to an ATA disk.
 *
 * We serialize the whole tree (directories + files) into one contiguous byte
 * image and write it sector-by-sector with the ATA driver. At boot we read it
 * back and rebuild the tree, so files survive reboots / power-off.
 *
 * The image is stored in a RESERVED REGION at the END of the disk (see
 * diskfs_base_lba), so it never collides with a bootloader (GRUB) at the start
 * of the disk. This means a single bootable disk/USB can BOTH boot the kernel
 * AND persist files on the same medium.
 *
 * On-disk image (little-endian):
 *   superblock:
 *     u32 magic        = 'M','K','F','S'  (0x53464B4D)
 *     u32 version      = 1
 *     u32 node_count
 *     u32 image_bytes  (total bytes incl. superblock)
 *   then node_count records, each:
 *     i32 parent_index  (-1 for the root)
 *     u32 type, permissions, owner_uid, owner_gid
 *     u32 data_size     (file payload length; 0 for dirs)
 *     u32 name_len
 *     name_len bytes (no NUL)
 *     data_size bytes  (file contents)
 */
#include "diskfs.h"
#include "vfs.h"
#include "ramfs.h"
#include "blockfs.h"
#include "../drivers/ata.h"
#include "../drivers/serial.h"
#include "../mm/kheap.h"
#include "../lib/string.h"

#define DISKFS_MAGIC    0x53464B4Du   /* "MKFS" */
#define DISKFS_VERSION  2u   /* v2: per-node flags + disk-backed block lists */
#define DISKFS_MAX_NODES 16384u
/* Max size of the serialized FS-metadata snapshot (tree + per-file headers).
 * This is NOT the cap on total file data — file *contents* live in blockfs
 * blocks anywhere on the disk, so the usable disk space scales with the disk
 * size. This cap only limits how big the metadata image at the tail can grow,
 * which in turn limits the total number/size of file entries (not bytes).
 * 256 MiB is plenty for tens of thousands of files. */
#define DISKFS_MAX_IMAGE (256u * 1024u * 1024u)

/* The persistence image lives in a reserved region at the END of the disk so
 * it never collides with a bootloader (GRUB) living at the start. This lets a
 * SINGLE bootable image both boot AND save files. The region is 8 MiB
 * (== DISKFS_MAX_IMAGE), i.e. 16384 sectors. */
#define DISKFS_TAIL_SECTORS  (DISKFS_MAX_IMAGE / ATA_SECTOR_SIZE)

/* First LBA of the persistence region. Normally the last 64 MiB of the disk.
 * The bootloader (GRUB + kernel ISO, ~10-16 MiB) lives at the START, so we
 * keep a 16 MiB boot reserve and never write below it. */
#define DISKFS_BOOT_RESERVE_SECTORS (16u * 1024u * 1024u / ATA_SECTOR_SIZE)

static uint32_t diskfs_base_lba(void)
{
    uint32_t total = ata_total_sectors();
    /* Preferred: the last DISKFS_TAIL_SECTORS of the disk. */
    if (total > DISKFS_TAIL_SECTORS + DISKFS_BOOT_RESERVE_SECTORS)
        return total - DISKFS_TAIL_SECTORS;
    /* Small disk: place right after the 16 MiB boot reserve (clears GRUB). */
    return DISKFS_BOOT_RESERVE_SECTORS;
}


/* ---- a tiny growable byte buffer (backed by the kernel heap) ---- */
typedef struct {
    uint8_t *buf;
    uint32_t len;
    uint32_t cap;
    bool     oom;
} buffer_t;

static void buf_ensure(buffer_t *b, uint32_t extra)
{
    if (b->oom)
        return;
    if (b->len + extra <= b->cap)
        return;
    uint32_t newcap = b->cap ? b->cap * 2 : 4096;
    while (newcap < b->len + extra)
        newcap *= 2;
    uint8_t *nb = (uint8_t *)krealloc(b->buf, newcap);
    if (!nb) { b->oom = true; return; }
    b->buf = nb;
    b->cap = newcap;
}

static void buf_put(buffer_t *b, const void *src, uint32_t n)
{
    buf_ensure(b, n);
    if (b->oom)
        return;
    memcpy(b->buf + b->len, src, n);
    b->len += n;
}

static void buf_u32(buffer_t *b, uint32_t v) { buf_put(b, &v, 4); }

/* ---- serialize: walk the tree in preorder ---- */

static void serialize_node(buffer_t *b, vfs_node_t *node, int32_t parent_idx,
                           vfs_node_t **order, int32_t *count)
{
    int32_t my_idx = *count;
    order[my_idx] = node;
    (*count)++;

    uint32_t name_len = (uint32_t)strlen(node->name);
    uint32_t data_size = (node->type == VFS_FILE) ? node->size : 0;
    bool disk = node->disk_backed && node->blocks;
    uint32_t flags = disk ? 1u : 0u;   /* bit0: disk-backed (blocks follow) */

    buf_put(b, &parent_idx, 4);
    buf_u32(b, node->type);
    buf_u32(b, node->permissions);
    buf_u32(b, node->owner_uid);
    buf_u32(b, node->owner_gid);
    buf_u32(b, data_size);
    buf_u32(b, name_len);
    buf_u32(b, flags);                  /* NEW in v2 */
    buf_put(b, node->name, name_len);
    if (disk) {
        /* store the block index list, not the bytes (bytes live on disk) */
        buf_u32(b, node->block_count);
        for (uint32_t i = 0; i < node->block_count; i++)
            buf_u32(b, node->blocks[i]);
    } else if (data_size && node->data) {
        buf_put(b, node->data, data_size);
    }

    for (uint32_t i = 0; i < node->child_count; i++) {
        if (*count >= (int32_t)DISKFS_MAX_NODES)
            return;
        serialize_node(b, node->children[i], my_idx, order, count);
    }
}

bool diskfs_available(void) { return ata_present(); }

uint32_t diskfs_total_bytes(void)
{
    if (!ata_present())
        return 0;
    return ata_total_sectors() * ATA_SECTOR_SIZE;
}

/* Recursively add up the serialized size of a node and its children, without
 * writing anything. Mirrors serialize_node(): 32 fixed header bytes (incl. the
 * v2 flags field) + name + either the block-index list (disk-backed) or the
 * inline payload (RAM-backed). */
static uint32_t measure_node(vfs_node_t *node)
{
    uint32_t bytes = 32;                        /* fixed record header (v2) */
    bytes += (uint32_t)strlen(node->name);      /* name */
    if (node->type == VFS_FILE) {
        if (node->disk_backed && node->blocks)
            bytes += 4 + node->block_count * 4; /* count + block indices */
        else
            bytes += node->size;                /* inline RAM payload */
    }
    for (uint32_t i = 0; i < node->child_count; i++)
        bytes += measure_node(node->children[i]);
    return bytes;
}

uint32_t diskfs_used_bytes(void)
{
    /* When block storage is active, the real consumption is the data blocks
     * holding file contents (4 KiB each), which is what users care about. */
    if (blockfs_available())
        return blockfs_used_blocks() * BLOCK_SIZE;

    vfs_node_t *root = ramfs_root();
    if (!root)
        return 0;
    return 16 /* superblock */ + measure_node(root);
}

int diskfs_save(void)
{
    if (!ata_present())
        return -1;

    vfs_node_t *root = ramfs_root();
    if (!root)
        return -2;

    vfs_node_t **order = (vfs_node_t **)kmalloc(
        sizeof(vfs_node_t *) * DISKFS_MAX_NODES);
    if (!order)
        return -3;

    buffer_t b = {0};
    /* reserve space for the superblock; fill it in at the end */
    buf_u32(&b, DISKFS_MAGIC);
    buf_u32(&b, DISKFS_VERSION);
    buf_u32(&b, 0);   /* node_count placeholder */
    buf_u32(&b, 0);   /* image_bytes placeholder */

    int32_t count = 0;
    serialize_node(&b, root, -1, order, &count);
    kfree(order);

    if (b.oom) { kfree(b.buf); return -4; }
    if (b.len > DISKFS_MAX_IMAGE) { kfree(b.buf); return -5; }

    /* patch superblock fields now that we know them */
    uint32_t node_count = (uint32_t)count;
    uint32_t image_bytes = b.len;
    memcpy(b.buf + 8,  &node_count, 4);
    memcpy(b.buf + 12, &image_bytes, 4);

    /* pad to a full sector and write */
    uint32_t sectors = (b.len + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    buf_ensure(&b, sectors * ATA_SECTOR_SIZE - b.len);
    if (b.oom) { kfree(b.buf); return -4; }
    /* zero the padding tail */
    if (sectors * ATA_SECTOR_SIZE > b.len)
        memset(b.buf + b.len, 0, sectors * ATA_SECTOR_SIZE - b.len);

    /* write to the reserved region at the end of the disk (so a bootloader at
     * the start is never overwritten). */
    uint32_t base = diskfs_base_lba();
    if (sectors > DISKFS_TAIL_SECTORS) { kfree(b.buf); return -5; }
    if (base + sectors > ata_total_sectors()) { kfree(b.buf); return -6; }

    /* write in chunks of up to 255 sectors per ATA command */
    uint32_t written = 0;
    while (written < sectors) {
        uint32_t chunk = sectors - written;
        if (chunk > 255) chunk = 255;
        if (ata_write_sectors(base + written, (uint8_t)chunk,
                              b.buf + written * ATA_SECTOR_SIZE) != 0) {
            kfree(b.buf);
            return -7;
        }
        written += chunk;
    }

    kfree(b.buf);
    /* also persist the block allocator bitmap so disk-backed file contents
     * are reachable after a reboot */
    if (blockfs_available())
        blockfs_flush_bitmap();

    serial_write("[diskfs] filesystem saved to disk\n");
    return 0;
}

/* ---- load: read the image and rebuild the tree ---- */

/* Recursively free every child of `node` (but keep `node` itself). */
static void wipe_children(vfs_node_t *node)
{
    for (uint32_t i = 0; i < node->child_count; i++) {
        vfs_node_t *c = node->children[i];
        wipe_children(c);
        if (c->data)
            kfree(c->data);
        kfree(c);
    }
    node->child_count = 0;
}

/* little-endian readers over a byte cursor */
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int diskfs_load(void)
{
    if (!ata_present())
        return -1;

    /* read the first sector of the reserved region to inspect the superblock */
    uint32_t base = diskfs_base_lba();
    uint8_t sb[ATA_SECTOR_SIZE];
    if (ata_read_sectors(base, 1, sb) != 0)
        return -2;

    if (rd_u32(sb + 0) != DISKFS_MAGIC)
        return 0;   /* no valid image -> caller keeps the default tree */
    if (rd_u32(sb + 4) != DISKFS_VERSION)
        return 0;

    uint32_t node_count  = rd_u32(sb + 8);
    uint32_t image_bytes = rd_u32(sb + 12);
    if (node_count == 0 || node_count > DISKFS_MAX_NODES)
        return -3;
    if (image_bytes < 16 || image_bytes > DISKFS_MAX_IMAGE)
        return -3;

    uint32_t sectors = (image_bytes + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    uint8_t *img = (uint8_t *)kmalloc(sectors * ATA_SECTOR_SIZE);
    if (!img)
        return -4;

    uint32_t got = 0;
    while (got < sectors) {
        uint32_t chunk = sectors - got;
        if (chunk > 255) chunk = 255;
        if (ata_read_sectors(base + got, (uint8_t)chunk,
                             img + got * ATA_SECTOR_SIZE) != 0) {
            kfree(img);
            return -5;
        }
        got += chunk;
    }

    vfs_node_t **map = (vfs_node_t **)kmalloc(
        sizeof(vfs_node_t *) * node_count);
    if (!map) { kfree(img); return -6; }

    /* fresh start: blow away the default tree, reuse the existing root node */
    vfs_node_t *root = ramfs_root();
    wipe_children(root);

    uint32_t off = 16;   /* skip superblock */
    for (uint32_t i = 0; i < node_count; i++) {
        if (off + 32 > image_bytes) { kfree(map); kfree(img); return -7; }
        int32_t  parent_idx = (int32_t)rd_u32(img + off + 0);
        uint32_t type       = rd_u32(img + off + 4);
        uint32_t perms      = rd_u32(img + off + 8);
        uint32_t uid        = rd_u32(img + off + 12);
        uint32_t gid        = rd_u32(img + off + 16);
        uint32_t data_size  = rd_u32(img + off + 20);
        uint32_t name_len   = rd_u32(img + off + 24);
        uint32_t flags      = rd_u32(img + off + 28);   /* v2 */
        off += 32;

        if (off + name_len > image_bytes || name_len >= 64) {
            kfree(map); kfree(img); return -7;
        }

        char name[64];
        memcpy(name, img + off, name_len);
        name[name_len] = '\0';
        off += name_len;

        vfs_node_t *node;
        if (parent_idx < 0) {
            /* the root: reuse the existing node, just restore its metadata */
            node = root;
            strncpy(node->name, name[0] ? name : "/", 63);
        } else {
            if (parent_idx >= (int32_t)i) { kfree(map); kfree(img); return -7; }
            vfs_node_t *parent = map[parent_idx];
            node = ramfs_create_node(parent, name, type);
            if (!node) { kfree(map); kfree(img); return -8; }
        }

        node->type        = type;
        node->permissions = perms;
        node->owner_uid   = uid;
        node->owner_gid   = gid;

        if (flags & 1u) {
            /* disk-backed: restore the block-index list, not the bytes */
            if (off + 4 > image_bytes) { kfree(map); kfree(img); return -7; }
            uint32_t bc = rd_u32(img + off);
            off += 4;
            if (off + bc * 4 > image_bytes) { kfree(map); kfree(img); return -7; }
            if (bc) {
                uint32_t *bl = (uint32_t *)kmalloc(bc * sizeof(uint32_t));
                if (!bl) { kfree(map); kfree(img); return -9; }
                for (uint32_t j = 0; j < bc; j++)
                    bl[j] = rd_u32(img + off + j * 4);
                node->blocks      = bl;
                node->block_count = bc;
            }
            node->disk_backed = true;
            node->size        = data_size;
            off += bc * 4;
        } else if (data_size) {
            if (off + data_size > image_bytes) { kfree(map); kfree(img); return -7; }
            uint8_t *d = (uint8_t *)kmalloc(data_size);
            if (!d) { kfree(map); kfree(img); return -9; }
            memcpy(d, img + off, data_size);
            node->data     = d;
            node->capacity = data_size;
            node->size     = data_size;
            off += data_size;
        }

        map[i] = node;
    }

    kfree(map);
    kfree(img);
    serial_write("[diskfs] filesystem loaded from disk\n");
    return 1;
}
