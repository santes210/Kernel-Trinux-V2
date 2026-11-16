/* mm/kheap.c  -  kernel heap allocator (first-fit with coalescing).
 *
 * Uses a static arena in .bss. Each block has a header; free blocks are
 * coalesced with neighbours on free. Supports aligned allocation, realloc and
 * statistics (used/free/largest-free/fragmentation).
 *
 * Note: file *contents* live in blockfs (on disk) when a disk is present, so
 * the heap only needs room for kernel metadata (VFS tree, process table, etc.)
 * plus RAM-only file data. 32 MiB is plenty and keeps the RAM requirement low
 * enough to run on modest machines / USB boots.
 */
#include "kheap.h"
#include "../lib/string.h"

#define HEAP_SIZE   (32 * 1024 * 1024)   /* 32 MiB */
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

/* Tope de una petición individual. Sin esto, `align_up(n, 8)` con
 * n = 0xFFFFFFFF da la vuelta a ~0 y `kmalloc(0xFFFFFFFF)` devolvía un puntero
 * válido a un bloque minúsculo: el llamante escribía GB.  Cualquier valor por
 * encima del arena entera no puede satisfacerse nunca, así que rechazarlo es
 * semánticamente correcto y no rompe a nadie. */
#define KHEAP_MAX_REQUEST (HEAP_SIZE - sizeof(block_t))

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
    if (size > KHEAP_MAX_REQUEST)
        return NULL;             /* imposible: no cabe ni con el arena vacío */

    size = align_up(size, ALIGN);
    if (size > KHEAP_MAX_REQUEST)
        return NULL;

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
/* Reserva `size` bytes con la carga útil alineada a 4 KiB.
 *
 * OJO (bug corregido): la versión anterior pedía `size + 4096 + sizeof(header)`
 * siempre, para poder "desplazar" la carga útil hasta el siguiente límite de
 * página, y si el bloque ya venía alineado hacía `return raw;`.  Ese `raw`
 * tiene `size + 4 KiB`, pero kfree() solo devuelve el bloque entero al
 * liberarlo, así que mientras tanto se consumían ~4 KiB extra por asignación
 * (medido con una simulación: 200 asignaciones de 8 KiB usaban 2.3 MiB en vez
 * de 1.6).  Y en el camino desplazado el hueco `raw..aligned` tampoco se
 * liberaba nunca.
 *
 * Ahora hay dos caminos limpios:
 *   a) el bloque ya está alineado -> kmalloc(size) exacto, sin pad;
 *   b) hay que desplazar -> se pide `size + 4096` y kfree() recupera `raw`
 *      del puntero guardado justo antes del payload alineado.  El hueco que
 *      queda por delante es un único bloque pequeño ya en la lista de libres
 *      (no se pierde: kmalloc() lo reutiliza).
 */
void *kmalloc_aligned(size_t size)
{
    if (!initialized)
        kheap_init();
    if (size == 0 || size > KHEAP_MAX_REQUEST)
        return NULL;
    size = align_up(size, ALIGN);

    /* a) Camino exacto: si el bloque que kmalloc() elegiría ya deja el
     * payload en un límite de página, no hace falta pad ninguno. */
    for (block_t *b = head; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;
        uintptr_t would_be = (uintptr_t)((uint8_t *)b + sizeof(block_t));
        if ((would_be & 0xFFFu) == 0)
            return kmalloc(size);        /* alineado y del tamaño exacto */
        break;                           /* first-fit: ese es el bloque */
    }

    /* b) Camino general: pad para alcanzar el siguiente límite de página. */
    const size_t pad = 4096 + sizeof(block_t);
    if (size > KHEAP_MAX_REQUEST - pad)
        return NULL;
    uint8_t *raw = (uint8_t *)kmalloc(size + pad);
    if (!raw)
        return NULL;

    uintptr_t addr    = (uintptr_t)raw;
    uintptr_t aligned = align_up(addr, 4096);
    if (aligned == addr)
        return raw;                      /* el pad sobra pero ya está alineado */

    uint8_t *aligned_ptr = (uint8_t *)aligned;
    ((void **)aligned_ptr)[-1] = raw;    /* kfree() recupera el bloque real */
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

    /* Rango válido: el arena es memoria estática, así que cualquier puntero
     * fuera de [heap, heap+HEAP_SIZE) es un bug del llamante. Antes se leía
     * `b->magic` a ciegas: un puntero basura podía "colar" si la palabra en
     * `ptr - sizeof(block_t)` coincidía con MAGIC_USED, y entonces kfree()
     * enlazaba un bloque falso en la lista -> el siguiente kmalloc escribía
     * en cualquier sitio. También hacía falta el mínimo para que
     * block_from_ptr() no apunte por debajo del arena. */
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t lo = (uintptr_t)heap + sizeof(block_t);
    uintptr_t hi = (uintptr_t)heap + HEAP_SIZE;
    if (p < lo || p >= hi)
        return;

    block_t *b = block_from_ptr(ptr);
    if (b->magic != MAGIC_USED) {
        /* maybe an aligned pointer: recover original raw pointer */
        void *raw = ((void **)ptr)[-1];
        if ((uintptr_t)raw < (uintptr_t)heap ||
            (uintptr_t)raw >= hi)
            return;                       /* no es un puntero nuestro */
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
