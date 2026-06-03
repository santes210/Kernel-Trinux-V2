#ifndef FS_FAT16_H
#define FS_FAT16_H

#include "../lib/types.h"
#include "vfs.h"

/* Initialize the FAT16 driver and mount it at /fat if a FAT16 partition is found. */
void fat16_init(void);

#endif
