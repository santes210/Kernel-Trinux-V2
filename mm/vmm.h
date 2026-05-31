#ifndef MM_VMM_H
#define MM_VMM_H

#include "../lib/types.h"

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

void vmm_init(void);
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void vmm_unmap_page(uint32_t virt);
bool vmm_is_enabled(void);

#endif /* MM_VMM_H */
