/* mm/pmm.c  -  physical memory manager (frame bitmap). */
#include "pmm.h"
#include "../lib/string.h"

#define MAX_FRAMES   (1024 * 1024)        /* up to 4 GiB / 4 KiB */
#define BITMAP_WORDS (MAX_FRAMES / 32)

static uint32_t frame_bitmap[BITMAP_WORDS];
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

    /* Mark everything used initially, then free what's available. */
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    used_frames = total_frames;

    /* Free all frames in range (they'll be re-reserved by reserve_region). */
    for (uint32_t i = 0; i < total_frames; i++) {
        bitmap_clear(i);
    }
    used_frames = 0;

    /* Reserve the first 128 MiB for BIOS/IVT/VGA + kernel + heap.
     * Kernel lives at 1 MiB; heap is 32 MiB; we add margin for safety.
     * The rest of the identity-mapped 1 GiB (128 MiB - 1 GiB) is available
     * for user processes and dynamic allocations. */
    pmm_reserve_region(0x00000000, 128 * 1024 * 1024);
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

/* Devuelve la dirección física de un frame libre, ya **puesto a cero**.
 *
 * Zeroear aquí (y no en cada llamante) arregla dos clases de bug:
 *   - el page directory / las page tables nuevas de vmm_create_address_space()
 *     se llenaban con basura del frame anterior -> entradas PAGE_PRESENT
 *     espurias y #PF aleatorios;
 *   - vmm_copy_region() en fork() podía "copiar" memoria de otro proceso.
 * El coste es un memset de 4 KiB por asignación, despreciable frente al
 * resto del camino (y los frames suelen venir ya a cero la primera vez).
 * Devuelve 0 si no hay memoria (0 nunca es un frame válido: ver pmm_init). */
uint32_t pmm_alloc_frame(void)
{
    for (uint32_t i = 1; i < total_frames; i++) {  /* el frame 0 está reservado */
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            uint32_t addr = i * PMM_FRAME_SIZE;
            memset((void *)addr, 0, PMM_FRAME_SIZE);
            return addr;
        }
    }
    return 0;   /* out of memory */
}

/* ¿Este frame físico está asignado actualmente?  Lo usan vmm_free_address_space()
 * (para no liberar page tables compartidas del kernel que nunca salieron del
 * PMM) y el chequeo defensivo de doble liberación. */
bool pmm_is_allocated(uint32_t addr)
{
    uint32_t frame = addr / PMM_FRAME_SIZE;
    if (frame >= total_frames) return false;
    return bitmap_test(frame);
}

void pmm_free_frame(uint32_t addr)
{
    uint32_t frame = addr / PMM_FRAME_SIZE;
    /* El frame 0 es el "null" del PMM: nunca se libera ni se entrega. */
    if (frame == 0) return;
    if ((addr % PMM_FRAME_SIZE) != 0) return;   /* dirección no alineada */
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
