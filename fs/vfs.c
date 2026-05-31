/* fs/vfs.c  -  virtual filesystem layer over ramfs. */
#include "vfs.h"
#include "ramfs.h"
#include "path.h"
#include "../lib/string.h"

vfs_node_t *vfs_root;

void vfs_init(void)
{
    /* ramfs_init() sets up the actual tree; called after this from kernel. */
    vfs_root = NULL;
}

vfs_node_t *vfs_get_root(void)
{
    if (!vfs_root)
        vfs_root = ramfs_root();
    return vfs_root;
}

/* ---- permissions ---- */

static vfs_cred_fn cred_provider;
static uint32_t    file_umask = 022;   /* default umask, like most shells */

void vfs_set_cred_provider(vfs_cred_fn fn) { cred_provider = fn; }
void vfs_set_umask(uint32_t mask)         { file_umask = mask & 0777; }
uint32_t vfs_get_umask(void)              { return file_umask; }

static vfs_cred_t current_cred(void)
{
    if (cred_provider)
        return cred_provider();
    /* default: root if no provider registered yet (e.g. early boot) */
    vfs_cred_t c = { 0, 0 };
    return c;
}

bool vfs_check_access(vfs_node_t *node, uint32_t mode)
{
    if (!node)
        return false;

    vfs_cred_t c = current_cred();
    if (c.uid == 0)
        return true;   /* root bypasses all checks */

    uint32_t perms = node->permissions;
    uint32_t bits;
    if (c.uid == node->owner_uid)
        bits = (perms >> 6) & 0x7;   /* owner */
    else if (c.gid == node->owner_gid)
        bits = (perms >> 3) & 0x7;   /* group */
    else
        bits = perms & 0x7;          /* others */

    return (bits & mode) == mode;
}

int vfs_chmod(const char *path, vfs_node_t *cwd, uint32_t perms)
{
    vfs_node_t *node = vfs_resolve(path, cwd);
    if (!node)
        return -1;
    vfs_cred_t c = current_cred();
    /* only owner or root may change permissions */
    if (c.uid != 0 && c.uid != node->owner_uid)
        return -2;
    node->permissions = perms & (0777 | VFS_STICKY);   /* keep sticky bit */
    return 0;
}

int vfs_chown(const char *path, vfs_node_t *cwd, uint32_t uid, uint32_t gid)
{
    vfs_node_t *node = vfs_resolve(path, cwd);
    if (!node)
        return -1;
    vfs_cred_t c = current_cred();
    /* only root may change ownership */
    if (c.uid != 0)
        return -2;
    node->owner_uid = uid;
    node->owner_gid = gid;
    return 0;
}

/* Reconstruct the absolute path of a node by walking parents. */
void vfs_get_path(vfs_node_t *node, char *out)
{
    if (!node || !node->parent) {
        strcpy(out, "/");
        return;
    }
    char rev[PATH_MAX] = "";
    char tmp[PATH_MAX] = "";
    vfs_node_t *n = node;
    while (n && n->parent) {
        char piece[VFS_NAME_MAX + 1];
        piece[0] = '/';
        strcpy(piece + 1, n->name);
        strcpy(tmp, piece);
        strcat(tmp, rev);
        strcpy(rev, tmp);
        n = n->parent;
    }
    strcpy(out, rev[0] ? rev : "/");
}

uint32_t vfs_read(vfs_node_t *node, uint32_t off, uint32_t size, uint8_t *buf)
{
    if (node && node->read)
        return node->read(node, off, size, buf);
    return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t off, uint32_t size, uint8_t *buf)
{
    if (node && node->write)
        return node->write(node, off, size, buf);
    return 0;
}

void vfs_open(vfs_node_t *node)  { if (node && node->open)  node->open(node); }
void vfs_close(vfs_node_t *node) { if (node && node->close) node->close(node); }

vfs_node_t *vfs_readdir(vfs_node_t *node, uint32_t index)
{
    if (node && node->readdir)
        return node->readdir(node, index);
    return NULL;
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name)
{
    if (node && node->finddir)
        return node->finddir(node, name);
    return NULL;
}

/* Build an absolute, normalized path from a (possibly relative) input. */
static void abs_normalize(const char *path, vfs_node_t *cwd, char *out)
{
    if (path_is_absolute(path)) {
        path_normalize("/", path, out);
    } else {
        char cwdpath[PATH_MAX];
        vfs_get_path(cwd ? cwd : vfs_get_root(), cwdpath);
        path_normalize(cwdpath, path, out);
    }
}

vfs_node_t *vfs_resolve(const char *path, vfs_node_t *cwd)
{
    char norm[PATH_MAX];
    abs_normalize(path, cwd, norm);

    vfs_node_t *cur = vfs_get_root();
    if (strcmp(norm, "/") == 0)
        return cur;

    char buf[PATH_MAX];
    strcpy(buf, norm);
    char *tok = strtok(buf + 1, "/");   /* skip leading slash */
    while (tok) {
        cur = vfs_finddir(cur, tok);
        if (!cur)
            return NULL;
        tok = strtok(NULL, "/");
    }
    return cur;
}

vfs_node_t *vfs_mkdir(const char *path, vfs_node_t *cwd)
{
    char norm[PATH_MAX], parent_path[PATH_MAX], name[PATH_MAX];
    abs_normalize(path, cwd, norm);
    path_dirname(norm, parent_path);
    path_basename(norm, name);

    vfs_node_t *parent = vfs_resolve(parent_path, NULL);
    if (!parent || parent->type != VFS_DIRECTORY)
        return NULL;
    if (!vfs_check_access(parent, ACC_WRITE))
        return NULL;   /* permission denied */
    if (vfs_finddir(parent, name))
        return NULL;   /* exists */
    vfs_node_t *n = ramfs_create_node(parent, name, VFS_DIRECTORY);
    if (n) {
        vfs_cred_t c = current_cred();
        n->owner_uid = c.uid;
        n->owner_gid = c.gid;
        n->permissions = 0777 & ~file_umask;   /* apply umask */
    }
    return n;
}

vfs_node_t *vfs_create(const char *path, vfs_node_t *cwd)
{
    char norm[PATH_MAX], parent_path[PATH_MAX], name[PATH_MAX];
    abs_normalize(path, cwd, norm);
    path_dirname(norm, parent_path);
    path_basename(norm, name);

    vfs_node_t *parent = vfs_resolve(parent_path, NULL);
    if (!parent || parent->type != VFS_DIRECTORY)
        return NULL;
    vfs_node_t *existing = vfs_finddir(parent, name);
    if (existing)
        return existing;
    if (!vfs_check_access(parent, ACC_WRITE))
        return NULL;   /* permission denied */
    vfs_node_t *n = ramfs_create_node(parent, name, VFS_FILE);
    if (n) {
        vfs_cred_t c = current_cred();
        n->owner_uid = c.uid;
        n->owner_gid = c.gid;
        n->permissions = 0666 & ~file_umask;   /* apply umask */
    }
    return n;
}

int vfs_delete(const char *path, vfs_node_t *cwd)
{
    vfs_node_t *node = vfs_resolve(path, cwd);
    if (!node)
        return -1;

    vfs_node_t *parent = node->parent;
    if (parent) {
        /* need write permission on the parent directory to unlink */
        if (!vfs_check_access(parent, ACC_WRITE))
            return -3;   /* permission denied */

        /* sticky bit: in a sticky dir only the file's owner, the dir's owner,
         * or root may delete an entry (classic /tmp protection). */
        if (parent->permissions & VFS_STICKY) {
            vfs_cred_t c = current_cred();
            if (c.uid != 0 &&
                c.uid != node->owner_uid &&
                c.uid != parent->owner_uid)
                return -4;   /* sticky: not permitted */
        }
    }
    return ramfs_remove_node(node);
}

int vfs_stat(const char *path, vfs_node_t *cwd, stat_t *st)
{
    vfs_node_t *node = vfs_resolve(path, cwd);
    if (!node)
        return -1;
    st->type        = node->type;
    st->size        = node->size;
    st->permissions = node->permissions;
    st->owner_uid   = node->owner_uid;
    st->owner_gid   = node->owner_gid;
    st->created     = node->created;
    st->modified    = node->modified;
    return 0;
}
