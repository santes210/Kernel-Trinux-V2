/* mm/pmm.c  -  physical memory manager (frame bitmap). */
#include "pmm.h"
#include "../lib/string.h"

#define MAX_FRAMES   (1024 * 1024)        /* up to 4 GiB / 4 KiB */
#define BITMAP_WORDS (MAX_FRAMES / 32)

/* El VMM solo identity-mapea el primer 1 GiB físico. Nunca entregar frames
 * por encima: kheap/vmm/fork escriben a phys como si fuera virt (phys==virt)
 * y un frame > 1 GiB causaría un page fault en ring 0. */
#define MAX_MAPPED_FRAMES  (0x40000000u / PMM_FRAME_SIZE)   /* 1 GiB */

static uint32_t frame_bitmap[BITMAP_WORDS];
/* ---- Copy-on-Write refcounts (v0.6.1) ----
 * Array paralelo al bitmap: frames posibles en [0x10000000, 0x40000000).
 * uint16_t por si aparecen cadenas de fork largas (nietos, bisnietos).
 * 196608 entradas * 2 bytes = 384 KiB de BSS (estático, cero al boot). */
#define COW_MAX_FRAMES  ((0x40000000u - PMM_COW_BASE) / PMM_FRAME_SIZE)

static uint16_t cow_refcount[COW_MAX_FRAMES];

static uint32_t total_frames;
static uint32_t used_frames;

static void bitmap_set(uint32_t frame)
{
    frame_bitmap[frame / 32] |= (1u << (frame % 32));
}

static void bitmap_clear(uint32_t frame)
{
    frame_bitmap[frame / 32] &= ~(1u << (frame % 32));
}

static bool bitmap_test(uint32_t frame)
{
    return (frame_bitmap[frame / 32] & (1u << (frame % 32))) != 0;
}

void pmm_init(uint32_t total_memory_bytes)
{
    total_frames = total_memory_bytes / PMM_FRAME_SIZE;
    if (total_frames > MAX_FRAMES)
        total_frames = MAX_FRAMES;
    if (total_frames > MAX_MAPPED_FRAMES)
        total_frames = MAX_MAPPED_FRAMES;

    /* Mark everything used initially, then free what's available. */
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    used_frames = total_frames;

    /* Free all frames in range (they'll be re-reserved by reserve_region). */
    for (uint32_t i = 0; i < total_frames; i++) {
        bitmap_clear(i);
    }
    used_frames = 0;

    /* Reserve los primeros 256 MiB: BIOS/kernel/heap + TODA la región de
     * usuario identity-mapped (ELFs a 0x08048000, stacks 0x0E800000-
     * 0x0F000000, heaps vía SYS_BRK hasta 0x0E000000).
     *
     * CRITICAL FIX (v0.5.3): antes solo se reservaban 128 MiB, así que los
     * frames físicos que respaldan código/stack/heap de los procesos de
     * usuario quedaban marcados LIBRES y pmm_alloc_frame() podía entregarlos
     * a page directories, page tables o a vmm_copy_region() de fork —
     * sobrescribiendo procesos vivos. Los frames dinámicos ahora provienen
     * exclusivamente de >= 256 MiB, región que ningún proceso usa como VA
     * (USER_SPACE_END = 0x10000000). En máquinas con <= 256 MiB
     * pmm_alloc_frame devuelve 0 y los callers degradan con gracia. */
    pmm_reserve_region(0x00000000, 256 * 1024 * 1024);

    /* v0.6.1: el array de refcounts COW es BSS — y Trinux NO zeroiza BSS
     * en boot.asm (funciona de casualidad en QEMU, que arranca la RAM a
     * cero). Inicializarlo explícitamente aquí: es la base de TODA la
     * contabilidad Copy-on-Write. */
    memset(cow_refcount, 0, sizeof(cow_refcount));
}

void pmm_reserve_region(uint32_t addr, uint32_t len)
{
    uint32_t start = addr / PMM_FRAME_SIZE;
    uint32_t end   = (addr + len + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    for (uint32_t f = start; f < end && f < total_frames; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
        }
    }
}

uint32_t pmm_alloc_frame(void)
{
    for (uint32_t i = 0; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            return i * PMM_FRAME_SIZE;
        }
    }
    return 0;   /* out of memory */
}

void pmm_free_frame(uint32_t addr)
{
    uint32_t frame = addr / PMM_FRAME_SIZE;
    if (frame < total_frames && bitmap_test(frame)) {
        bitmap_clear(frame);
        if (used_frames > 0)
            used_frames--;
    }
}

uint32_t pmm_get_total_memory(void) { return total_frames * PMM_FRAME_SIZE; }
uint32_t pmm_get_used_memory(void)  { return used_frames * PMM_FRAME_SIZE; }
uint32_t pmm_get_free_memory(void)
{
    return (total_frames - used_frames) * PMM_FRAME_SIZE;
}

static bool cow_trackable(uint32_t phys)
{
    return phys >= PMM_COW_BASE && phys < 0x40000000u;
}

uint32_t pmm_cow_share(uint32_t phys)
{
    if (!cow_trackable(phys)) return 0;
    uint32_t idx = (phys - PMM_COW_BASE) / PMM_FRAME_SIZE;
    if (cow_refcount[idx] == 0)
        cow_refcount[idx] = 2;   /* pasa de privado a compartido (1+1) */
    else if (cow_refcount[idx] < 0xFFFF)
        cow_refcount[idx]++;
    return cow_refcount[idx];
}

uint32_t pmm_cow_refs(uint32_t phys)
{
    if (!cow_trackable(phys)) return 0;
    return cow_refcount[(phys - PMM_COW_BASE) / PMM_FRAME_SIZE];
}

uint32_t pmm_cow_unshare(uint32_t phys)
{
    if (!cow_trackable(phys)) return 0;
    uint32_t idx = (phys - PMM_COW_BASE) / PMM_FRAME_SIZE;
    if (cow_refcount[idx] > 0)
        cow_refcount[idx]--;
    return cow_refcount[idx];
}
