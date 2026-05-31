#ifndef FS_VFS_H
#define FS_VFS_H

#include "../lib/types.h"

#define VFS_FILE      0x01
#define VFS_DIRECTORY 0x02
#define VFS_DEVICE    0x04

#define VFS_NAME_MAX  64
#define VFS_MAX_CHILDREN 64

struct vfs_node;

typedef uint32_t (*read_fn)(struct vfs_node *, uint32_t off, uint32_t size, uint8_t *buf);
typedef uint32_t (*write_fn)(struct vfs_node *, uint32_t off, uint32_t size, uint8_t *buf);
typedef void     (*open_fn)(struct vfs_node *);
typedef void     (*close_fn)(struct vfs_node *);
typedef struct vfs_node *(*readdir_fn)(struct vfs_node *, uint32_t index);
typedef struct vfs_node *(*finddir_fn)(struct vfs_node *, const char *name);

typedef struct vfs_node {
    char     name[VFS_NAME_MAX];
    uint32_t type;
    uint32_t permissions;     /* unix-like rwx bits, e.g. 0755 */
    uint32_t owner_uid;       /* owning user id */
    uint32_t owner_gid;       /* owning group id */
    uint32_t size;            /* bytes for files */
    uint32_t created;         /* unix-ish timestamp (seconds since boot proxy) */
    uint32_t modified;

    read_fn    read;
    write_fn   write;
    open_fn    open;
    close_fn   close;
    readdir_fn readdir;
    finddir_fn finddir;

    struct vfs_node *parent;
    struct vfs_node *children[VFS_MAX_CHILDREN];
    uint32_t         child_count;

    uint8_t  *data;           /* file contents (ramfs, RAM-backed mode) */
    uint32_t  capacity;       /* allocated capacity of data */

    /* Disk-backed content (when a disk is present): the file's bytes live in
     * 4 KiB blocks on disk instead of in RAM. `blocks` is a RAM-resident list
     * of disk block indices; `block_count` is how many are allocated. When
     * disk_backed is false the node uses the `data`/`capacity` RAM buffer. */
    uint32_t *blocks;
    uint32_t  block_count;
    bool      disk_backed;
} vfs_node_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t permissions;
    uint32_t owner_uid;
    uint32_t owner_gid;
    uint32_t created;
    uint32_t modified;
} stat_t;

/* Access modes for vfs_check_access(). */
#define ACC_READ   0x4
#define ACC_WRITE  0x2
#define ACC_EXEC   0x1

extern vfs_node_t *vfs_root;

void        vfs_init(void);
vfs_node_t *vfs_get_root(void);

uint32_t    vfs_read(vfs_node_t *node, uint32_t off, uint32_t size, uint8_t *buf);
uint32_t    vfs_write(vfs_node_t *node, uint32_t off, uint32_t size, uint8_t *buf);
void        vfs_open(vfs_node_t *node);
void        vfs_close(vfs_node_t *node);
vfs_node_t *vfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name);

/* Path-based helpers (resolve through ramfs). */
vfs_node_t *vfs_resolve(const char *path, vfs_node_t *cwd);
void        vfs_get_path(vfs_node_t *node, char *out);   /* absolute path */
vfs_node_t *vfs_mkdir(const char *path, vfs_node_t *cwd);
vfs_node_t *vfs_create(const char *path, vfs_node_t *cwd);
int         vfs_delete(const char *path, vfs_node_t *cwd);
int         vfs_stat(const char *path, vfs_node_t *cwd, stat_t *st);

/* ---- permissions ----
 * The auth layer registers a provider so the VFS can know who is logged in
 * without a hard fs->auth dependency. uid 0 (root) bypasses all checks. */
typedef struct { uint32_t uid; uint32_t gid; } vfs_cred_t;
typedef vfs_cred_t (*vfs_cred_fn)(void);
void        vfs_set_cred_provider(vfs_cred_fn fn);

/* Returns true if the current user may access `node` with the given mode
 * (ACC_READ/ACC_WRITE/ACC_EXEC, OR-able). */
bool        vfs_check_access(vfs_node_t *node, uint32_t mode);
int         vfs_chmod(const char *path, vfs_node_t *cwd, uint32_t perms);
int         vfs_chown(const char *path, vfs_node_t *cwd, uint32_t uid, uint32_t gid);

/* Permission mask applied to newly created files/dirs (default 022). */
void        vfs_set_umask(uint32_t mask);
uint32_t    vfs_get_umask(void);

/* Sticky bit (chmod 1xxx): in a sticky dir, only a file's owner (or the dir's
 * owner, or root) may delete/rename entries — like /tmp. */
#define VFS_STICKY 01000

#endif /* FS_VFS_H */
