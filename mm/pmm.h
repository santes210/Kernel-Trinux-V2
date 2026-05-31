#ifndef MM_PMM_H
#define MM_PMM_H

#include "../lib/types.h"

#define PMM_FRAME_SIZE 4096

void     pmm_init(uint32_t total_memory_bytes);
uint32_t pmm_alloc_frame(void);              /* returns physical addr or 0 */
void     pmm_free_frame(uint32_t addr);
uint32_t pmm_get_free_memory(void);          /* bytes */
uint32_t pmm_get_used_memory(void);          /* bytes */
uint32_t pmm_get_total_memory(void);         /* bytes */
void     pmm_reserve_region(uint32_t addr, uint32_t len);

#endif /* MM_PMM_H */
