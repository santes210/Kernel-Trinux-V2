/* fs/ramfs.c  -  filesystem backing the VFS.
 *
 * Two storage modes per file:
 *   - RAM mode  : contents in a kheap buffer (node->data). Used when there is
 *                 no disk, or for the tiny default files created at boot.
 *   - DISK mode : contents in 4 KiB disk blocks (node->blocks[]), read/written
 *                 on demand via blockfs. Used for files created while a disk is
 *                 present, so file data does not consume kernel heap and total
 *                 storage is bounded by the disk, not by RAM.
 */
#include "ramfs.h"
#include "blockfs.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../drivers/timer.h"

static vfs_node_t *root;

/* ---- disk-backed helpers ---- */

/* Ensure the node has at least `nblocks` blocks allocated. Returns false on
 * out-of-space. Newly grown blocks are zero-filled on disk. */
static bool ensure_blocks(vfs_node_t *node, uint32_t nblocks)
{
    if (nblocks <= node->block_count)
        return true;

    uint32_t *nb = (uint32_t *)krealloc(node->blocks,
                                        nblocks * sizeof(uint32_t));
    if (!nb)
        return false;
    node->blocks = nb;

    static uint8_t zero[BLOCK_SIZE];
    while (node->block_count < nblocks) {
        uint32_t b = blockfs_alloc();
        if (b == BLOCK_INVALID)
            return false;   /* disk full */
        memset(zero, 0, BLOCK_SIZE);
        blockfs_write_block(b, zero);
        node->blocks[node->block_count++] = b;
    }
    return true;
}

static uint32_t diskfile_read(vfs_node_t *node, uint32_t off, uint32_t size,
                              uint8_t *buf)
{
    if (off >= node->size)
        return 0;
    uint32_t avail = node->size - off;
    if (size > avail)
        size = avail;

    uint8_t blk[BLOCK_SIZE];
    uint32_t done = 0;
    while (done < size) {
        uint32_t pos    = off + done;
        uint32_t bindex = pos / BLOCK_SIZE;
        uint32_t boff   = pos % BLOCK_SIZE;
        uint32_t chunk  = BLOCK_SIZE - boff;
        if (chunk > size - done) chunk = size - done;

        if (bindex >= node->block_count) break;
        if (blockfs_read_block(node->blocks[bindex], blk) != 0) break;
        memcpy(buf + done, blk + boff, chunk);
        done += chunk;
    }
    return done;
}

static uint32_t diskfile_write(vfs_node_t *node, uint32_t off, uint32_t size,
                               uint8_t *buf)
{
    uint32_t needed = off + size;
    uint32_t nblocks = (needed + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (!ensure_blocks(node, nblocks))
        return 0;

    uint8_t blk[BLOCK_SIZE];
    uint32_t done = 0;
    while (done < size) {
        uint32_t pos    = off + done;
        uint32_t bindex = pos / BLOCK_SIZE;
        uint32_t boff   = pos % BLOCK_SIZE;
        uint32_t chunk  = BLOCK_SIZE - boff;
        if (chunk > size - done) chunk = size - done;

        /* read-modify-write the block (partial writes must preserve the rest) */
        if (chunk != BLOCK_SIZE)
            blockfs_read_block(node->blocks[bindex], blk);
        memcpy(blk + boff, buf + done, chunk);
        if (blockfs_write_block(node->blocks[bindex], blk) != 0)
            break;
        done += chunk;
    }

    if (off + done > node->size)
        node->size = off + done;
    node->modified = uptime();
    return done;
}

/* ---- file op callbacks ---- */

static uint32_t ramfs_read(vfs_node_t *node, uint32_t off, uint32_t size,
                           uint8_t *buf)
{
    if (node->disk_backed)
        return diskfile_read(node, off, size, buf);

    if (!node->data || off >= node->size)
        return 0;
    uint32_t avail = node->size - off;
    if (size > avail)
        size = avail;
    memcpy(buf, node->data + off, size);
    return size;
}

static uint32_t ramfs_write(vfs_node_t *node, uint32_t off, uint32_t size,
                            uint8_t *buf)
{
    /* Promote to disk-backed storage on first write if a disk is available
     * and this node has no RAM data yet. Tiny boot files stay in RAM. */
    if (!node->disk_backed && node->capacity == 0 && blockfs_available())
        node->disk_backed = true;

    if (node->disk_backed)
        return diskfile_write(node, off, size, buf);

    uint32_t needed = off + size;
    if (needed > node->capacity) {
        uint32_t newcap = needed + 64;
        uint8_t *nd = (uint8_t *)krealloc(node->data, newcap);
        if (!nd)
            return 0;
        node->data = nd;
        node->capacity = newcap;
    }
    memcpy(node->data + off, buf, size);
    if (needed > node->size)
        node->size = needed;
    node->modified = uptime();
    return size;
}

static vfs_node_t *ramfs_readdir(vfs_node_t *node, uint32_t index)
{
    if (node->type != VFS_DIRECTORY || index >= node->child_count)
        return NULL;
    return node->children[index];
}

static vfs_node_t *ramfs_finddir(vfs_node_t *node, const char *name)
{
    if (node->type != VFS_DIRECTORY)
        return NULL;
    for (uint32_t i = 0; i < node->child_count; i++) {
        if (strcmp(node->children[i]->name, name) == 0)
            return node->children[i];
    }
    return NULL;
}

/* ---- node management ---- */

vfs_node_t *ramfs_create_node(vfs_node_t *parent, const char *name,
                              uint32_t type)
{
    if (parent && parent->child_count >= VFS_MAX_CHILDREN)
        return NULL;

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node)
        return NULL;
    memset(node, 0, sizeof(vfs_node_t));

    strncpy(node->name, name, VFS_NAME_MAX - 1);
    node->type        = type;
    node->permissions = (type == VFS_DIRECTORY) ? 0755 : 0644;
    node->created     = uptime();
    node->modified    = node->created;
    node->read        = ramfs_read;
    node->write       = ramfs_write;
    node->readdir     = ramfs_readdir;
    node->finddir     = ramfs_finddir;
    node->parent      = parent;

    if (parent)
        parent->children[parent->child_count++] = node;

    return node;
}

int ramfs_remove_node(vfs_node_t *node)
{
    if (!node || !node->parent)
        return -1;
    if (node->type == VFS_DIRECTORY && node->child_count > 0)
        return -2;   /* not empty */

    vfs_node_t *p = node->parent;
    uint32_t idx = (uint32_t)-1;
    for (uint32_t i = 0; i < p->child_count; i++) {
        if (p->children[i] == node) {
            idx = i;
            break;
        }
    }
    if (idx == (uint32_t)-1)
        return -1;

    /* shift remaining children */
    for (uint32_t i = idx; i + 1 < p->child_count; i++)
        p->children[i] = p->children[i + 1];
    p->child_count--;

    /* free disk blocks (if disk-backed) and any RAM buffer / block list */
    if (node->disk_backed && node->blocks) {
        for (uint32_t i = 0; i < node->block_count; i++)
            blockfs_free(node->blocks[i]);
    }
    if (node->blocks)
        kfree(node->blocks);
    if (node->data)
        kfree(node->data);
    kfree(node);
    return 0;
}

vfs_node_t *ramfs_root(void) { return root; }

static void write_string_file(vfs_node_t *node, const char *text)
{
    ramfs_write(node, 0, (uint32_t)strlen(text), (uint8_t *)text);
}

void ramfs_init(void)
{
    root = ramfs_create_node(NULL, "/", VFS_DIRECTORY);

    vfs_node_t *bin  = ramfs_create_node(root, "bin", VFS_DIRECTORY);
    vfs_node_t *etc  = ramfs_create_node(root, "etc", VFS_DIRECTORY);
    vfs_node_t *home = ramfs_create_node(root, "home", VFS_DIRECTORY);
    vfs_node_t *tmp  = ramfs_create_node(root, "tmp", VFS_DIRECTORY);
    vfs_node_t *var  = ramfs_create_node(root, "var", VFS_DIRECTORY);
    ramfs_create_node(root, "dev", VFS_DIRECTORY);
    /* Standard Unix mount point — many programs (and users) expect /mnt to
     * exist out of the box, even if nothing is mounted there yet. */
    ramfs_create_node(root, "mnt", VFS_DIRECTORY);
    /* /root is normally created by users_init() if missing, but having it
     * here too means it's available even before users init runs. */
    vfs_node_t *root_home = ramfs_create_node(root, "root", VFS_DIRECTORY);
    if (root_home) { root_home->owner_uid = 0; root_home->owner_gid = 0;
                     root_home->permissions = 0700; }
    (void)bin;

    /* /tmp is world-writable with the sticky bit (1777), like a real Unix
     * system: anyone can create files there, but only delete their own. */
    if (tmp) tmp->permissions = 0777 | VFS_STICKY;

    /* /home/user belongs to the default user (uid 1000, matching /etc/passwd) */
    vfs_node_t *u_home = ramfs_create_node(home, "user", VFS_DIRECTORY);
    if (u_home) { u_home->owner_uid = 1000; u_home->owner_gid = 1000; }

    ramfs_create_node(var, "log", VFS_DIRECTORY);

    vfs_node_t *hostname = ramfs_create_node(etc, "hostname", VFS_FILE);
    write_string_file(hostname, "trinux\n");

    vfs_node_t *motd = ramfs_create_node(etc, "motd", VFS_FILE);
    write_string_file(motd,
        "Welcome to Trinux!\n"
        "A tiny Unix-like kernel written in C and assembly.\n"
        "Type 'help' to get started.\n");

    /* a sample file in the user's home, owned by the user */
    vfs_node_t *home_user = ramfs_finddir(home, "user");
    if (home_user) {
        vfs_node_t *readme = ramfs_create_node(home_user, "readme.txt", VFS_FILE);
        readme->owner_uid = 1000;
        readme->owner_gid = 1000;
        write_string_file(readme,
            "Hello from Trinux.\n"
            "This file lives in a RAM filesystem.\n"
            "Try: cat readme.txt, wc readme.txt, hexdump readme.txt\n");
    }
}
