/* mm/pmm.c  -  physical memory manager (frame bitmap). */
#include "pmm.h"
#include "../lib/string.h"

#define MAX_FRAMES   (1024 * 1024)        /* up to 4 GiB / 4 KiB */
#define BITMAP_WORDS (MAX_FRAMES / 32)

static uint32_t frame_bitmap[BITMAP_WORDS];
static uint32_t total_frames;
static uint32_t used_frames;
/* Cursor de asignación: recuerda el primer frame que podría estar libre.
 * Sin él, cada pmm_alloc_frame() reescaneaba desde el frame 0 (O(n) sobre
 * toda la RAM aunque el sistema estuviera fresco). El cursor solo avanza,
 * así que la asignación sucesiva es amortiguada O(1); los frames liberados
 * por debajo del cursor lo rebobinan para no perderlos. */
static uint32_t alloc_hint;

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

    /* La reserva deja los primeros frames usados; arrancamos el cursor al
     * final de esa región para no reescanear 32768 frames en cada arranque. */
    alloc_hint = (128u * 1024u * 1024u) / PMM_FRAME_SIZE;
    if (alloc_hint >= total_frames) alloc_hint = 0;
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
    /* Pasada 1: desde el cursor (caso normal, casi siempre acierta de
     * inmediato → coste amortiguado O(1) frente al O(n) del scan desde 0). */
    for (uint32_t i = alloc_hint; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            alloc_hint = i + 1;
            return i * PMM_FRAME_SIZE;
        }
    }
    /* Pasada 2: puede haber frames libres por delante del cursor debido a
     * liberaciones (free rebobina el hint, pero tras varios alloc/free el
     * hueco puede quedar atrás). Barrido completo de rescate. */
    for (uint32_t i = 0; i < total_frames && i < alloc_hint; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            alloc_hint = i + 1;
            return i * PMM_FRAME_SIZE;
        }
    }
    /* Sin RAM. El frame físico 0 nunca es asignable (los primeros 128 MiB
     * están reservados en pmm_init), así que 0 sigue siendo un centinela
     * inequívoco de "out of memory". */
    return 0;
}

void pmm_free_frame(uint32_t addr)
{
    uint32_t frame = addr / PMM_FRAME_SIZE;
    if (frame < total_frames && bitmap_test(frame)) {
        bitmap_clear(frame);
        if (used_frames > 0)
            used_frames--;
        /* Rebobinar el cursor para que un alloc posterior encuentre el hueco
         * sin necesidad de la segunda pasada. */
        if (frame < alloc_hint)
            alloc_hint = frame;
    }
}

uint32_t pmm_get_total_memory(void) { return total_frames * PMM_FRAME_SIZE; }
uint32_t pmm_get_used_memory(void)  { return used_frames * PMM_FRAME_SIZE; }
uint32_t pmm_get_free_memory(void)
{
    return (total_frames - used_frames) * PMM_FRAME_SIZE;
}
