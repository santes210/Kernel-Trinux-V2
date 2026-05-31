/* mm/kheap.c  -  kernel heap allocator (first-fit with coalescing).
 *
 * Uses a static 4 MiB arena in .bss. Each block has a header; free blocks are
 * coalesced with neighbours on free. Supports aligned allocation, realloc and
 * statistics (used/free/largest-free/fragmentation).
 */
#include "kheap.h"
#include "../lib/string.h"

#define HEAP_SIZE   (96 * 1024 * 1024)   /* 96 MiB */
#define ALIGN       8
#define MAGIC_USED  0xA110C8ED
#define MAGIC_FREE  0xF8EE8100

typedef struct block {
    uint32_t      magic;
    size_t        size;     /* usable payload size */
    bool          free;
    struct block *next;
    struct block *prev;
} block_t;

static uint8_t  heap[HEAP_SIZE] __attribute__((aligned(4096)));
static block_t *head;
static bool     initialized;

static size_t align_up(size_t n, size_t a)
{
    return (n + (a - 1)) & ~(a - 1);
}

void kheap_init(void)
{
    head = (block_t *)heap;
    head->magic = MAGIC_FREE;
    head->size  = HEAP_SIZE - sizeof(block_t);
    head->free  = true;
    head->next  = NULL;
    head->prev  = NULL;
    initialized = true;
}

/* Split a block if it is much larger than requested. */
static void split_block(block_t *b, size_t size)
{
    if (b->size >= size + sizeof(block_t) + ALIGN) {
        block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
        nb->magic = MAGIC_FREE;
        nb->size  = b->size - size - sizeof(block_t);
        nb->free  = true;
        nb->next  = b->next;
        nb->prev  = b;
        if (b->next)
            b->next->prev = nb;
        b->next = nb;
        b->size = size;
    }
}

void *kmalloc(size_t size)
{
    if (!initialized)
        kheap_init();
    if (size == 0)
        return NULL;

    size = align_up(size, ALIGN);

    for (block_t *b = head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free  = false;
            b->magic = MAGIC_USED;
            return (void *)((uint8_t *)b + sizeof(block_t));
        }
    }
    return NULL;   /* out of memory */
}

void *kcalloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *p = kmalloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

/* Page-aligned allocation: over-allocate and carve an aligned region. */
void *kmalloc_aligned(size_t size)
{
    if (!initialized)
        kheap_init();
    size = align_up(size, ALIGN);

    /* allocate enough to find a 4K-aligned payload */
    size_t pad = 4096 + sizeof(block_t);
    uint8_t *raw = (uint8_t *)kmalloc(size + pad);
    if (!raw)
        return NULL;

    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = align_up(addr, 4096);
    if (aligned == addr)
        return raw;   /* already aligned */

    /* We can't easily move the block header, so store original ptr just before
     * the aligned address for kfree to recover. */
    uint8_t *aligned_ptr = (uint8_t *)aligned;
    ((void **)aligned_ptr)[-1] = raw;
    return aligned_ptr;
}

static block_t *block_from_ptr(void *ptr)
{
    return (block_t *)((uint8_t *)ptr - sizeof(block_t));
}

static void coalesce(block_t *b)
{
    /* merge with next */
    if (b->next && b->next->free) {
        b->size += sizeof(block_t) + b->next->size;
        b->next = b->next->next;
        if (b->next)
            b->next->prev = b;
    }
    /* merge with prev */
    if (b->prev && b->prev->free) {
        b->prev->size += sizeof(block_t) + b->size;
        b->prev->next = b->next;
        if (b->next)
            b->next->prev = b->prev;
    }
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    /* Detect aligned allocation (header is recorded just before payload and
     * payload is not immediately after a valid block header). */
    block_t *b = block_from_ptr(ptr);
    if (b->magic != MAGIC_USED) {
        /* maybe an aligned pointer: recover original raw pointer */
        void *raw = ((void **)ptr)[-1];
        block_t *rb = block_from_ptr(raw);
        if (rb->magic == MAGIC_USED) {
            rb->free  = true;
            rb->magic = MAGIC_FREE;
            coalesce(rb);
        }
        return;
    }

    b->free  = true;
    b->magic = MAGIC_FREE;
    coalesce(b);
}

void *krealloc(void *ptr, size_t new_size)
{
    if (!ptr)
        return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    block_t *b = block_from_ptr(ptr);
    if (b->magic == MAGIC_USED && b->size >= new_size)
        return ptr;   /* already big enough */

    void *np = kmalloc(new_size);
    if (!np)
        return NULL;

    size_t copy = (b->magic == MAGIC_USED && b->size < new_size)
                      ? b->size : new_size;
    memcpy(np, ptr, copy);
    kfree(ptr);
    return np;
}

void kheap_stats(size_t *used, size_t *free, size_t *largest_free,
                 uint32_t *block_count)
{
    size_t u = 0, f = 0, lf = 0;
    uint32_t bc = 0;
    for (block_t *b = head; b; b = b->next) {
        bc++;
        if (b->free) {
            f += b->size;
            if (b->size > lf)
                lf = b->size;
        } else {
            u += b->size;
        }
    }
    if (used)         *used = u;
    if (free)         *free = f;
    if (largest_free) *largest_free = lf;
    if (block_count)  *block_count = bc;
}
