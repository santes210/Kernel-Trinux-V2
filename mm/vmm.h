#ifndef MM_VMM_H
#define MM_VMM_H

#include "../lib/types.h"

#define PAGE_PRESENT       0x1
#define PAGE_RW            0x2
#define PAGE_USER          0x4
#define PAGE_WRITE_THROUGH 0x8
#define PAGE_CACHE_DISABLE 0x10

void vmm_init(void);
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void vmm_unmap_page(uint32_t virt);
bool vmm_is_enabled(void);

uint32_t vmm_create_address_space(void);
void     vmm_switch_address_space(uint32_t pd_phys);
void     vmm_free_address_space(uint32_t pd_phys);
void     vmm_map_page_in(uint32_t pd_phys, uint32_t virt, uint32_t phys, uint32_t flags);
uint32_t vmm_get_current_dir(void);

#endif /* MM_VMM_H */
