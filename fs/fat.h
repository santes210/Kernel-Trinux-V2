#ifndef FS_FAT_H
#define FS_FAT_H

#include "../lib/types.h"
#include "vfs.h"

/* Initialize the FAT16/FAT32 driver and mount it at /fat if a partition is found. */
void fat_init(void);

#endif
