#ifndef KERNEL_MULTIBOOT_H
#define KERNEL_MULTIBOOT_H

#include "../lib/types.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

typedef struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;     /* KiB below 1 MiB */
    uint32_t mem_upper;     /* KiB above 1 MiB */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    /* ... more fields exist but unused */
} __attribute__((packed)) multiboot_info_t;

typedef struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;          /* 1 = available */
} __attribute__((packed)) multiboot_mmap_entry_t;

#define MULTIBOOT_FLAG_MEM  0x001
#define MULTIBOOT_FLAG_MMAP 0x040
#define MULTIBOOT_MEMORY_AVAILABLE 1

#endif /* KERNEL_MULTIBOOT_H */
