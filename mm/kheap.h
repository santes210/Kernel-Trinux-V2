#ifndef MM_KHEAP_H
#define MM_KHEAP_H

#include "../lib/types.h"

void  kheap_init(void);
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size);
void *kcalloc(size_t count, size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void  kheap_stats(size_t *used, size_t *free, size_t *largest_free,
                  uint32_t *block_count);

#endif /* MM_KHEAP_H */
