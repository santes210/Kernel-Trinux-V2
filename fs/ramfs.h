#ifndef FS_RAMFS_H
#define FS_RAMFS_H

#include "vfs.h"

void        ramfs_init(void);
vfs_node_t *ramfs_create_node(vfs_node_t *parent, const char *name, uint32_t type);
int         ramfs_remove_node(vfs_node_t *node);
vfs_node_t *ramfs_root(void);

#endif /* FS_RAMFS_H */
